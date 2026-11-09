#include "ui/menu.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "engine.h"
#include "maps.h"
#include "route.h"
#include "trainer.h"
#include "ui/boards.h"
#include "ui/gfx.h"
#include "ui/hud.h"
#include "ui/library.h"
#include "ui/recents.h"
#include "ui/widgets.h"
#include "version.h"

namespace menu {
namespace {

using gfx::Color;
using gfx::Font;
using ui::Px;
using ui::Rect;
namespace theme = ui::theme;

enum class Page { Practice, Recent, Runs, Boards, Settings };
constexpr int kPages = 5;

struct Keys {
	const char *name;
	int code;
};
// (the first is the default)
const Keys kMenuKeys[] = {{"INSERT", gfx::kKeyInsert}, {"HOME", gfx::kKeyHome}, {"RSHIFT", gfx::kKeyRShift},
                          {"F1", gfx::kKeyF1},      {"F2", gfx::kKeyF1 + 1},  {"F3", gfx::kKeyF1 + 2},  {"F4", gfx::kKeyF1 + 3},
                          {"F5", gfx::kKeyF1 + 4},  {"F6", gfx::kKeyF1 + 5},  {"F7", gfx::kKeyF1 + 6},  {"F8", gfx::kKeyF1 + 7},
                          {"F9", gfx::kKeyF1 + 8},  {"F10", gfx::kKeyF1 + 9}, {"F11", gfx::kKeyF1 + 10}, {"F12", gfx::kKeyF1 + 11}};

struct State {
	bool ready = false;
	bool open = false;
	Page page = Page::Practice;
	int menuKey = 0;  // index into kMenuKeys
	// practice page
	std::string loadedKey;  // the run the page's choices were set for
	float takeOver = 0;
	int mode = 0, role = 0, partner = 0;  // practice/watch, blue/orange, bot/real partner
	int partsScroll = 0;
	// runs page
	std::string search, searchShown = "\x01";
	bool runsThisMap = false;  // only the runs of the level that is up
	int runsSort = 0;          // 0 by chamber (the game's order), 1 fastest first, 2 newest first
	int libraryVersion = -1;
	std::vector<int> shown;  // indices of the runs matching the search
	int runsScroll = 0;
	// (runs are known by their demo's path, not their place in the list: the list is made again, in another order,
	// whenever the folders are read)
	std::string selectedRun, lastClicked;
	bool foldersOpen = false;
	std::string newFolder, folderNote;
	DWORD lastClick = 0;
	// boards page
	std::string boardMap;
	std::string chamberFilter;
	bool scrollToChamber = false;  // bring the chosen chamber into view in the list
	int chaptersScroll = 0, boardScroll = 0, selectedEntry = -1;
	std::map<int, bool> downloaded;  // by changelog id, for the board shown
	bool downloading = false;
	std::string downloadMap, downloadName;  // the chamber the demo on its way was asked for: its map and name
	std::string boardNote;
	// recent page
	std::string recentKey;      // the loaded run last put on top of the recent list
	int recentScroll = 0, savedScroll = 0, selectedRecent = -1, selectedSaved = -1;
	bool choicesPending = false;  // a saved run is being loaded: its choices go on the practice page
	recents::Entry pendingChoices;
	std::string saveName;         // the practice page's name for a saved run
	std::string profileName;      // the settings page's name for saved settings
	std::string profileNote;
	int saveSettingsIn = 0;       // frames until the settings are written (after loading saved ones or dragging a slider)
	// messages along the bottom
	std::string note;
	DWORD noteUntil = 0;
};
State g;

void Note(const std::string &text, int seconds = 6) {
	g.note = text;
	g.noteUntil = GetTickCount() + seconds * 1000;
}

std::string Lower(std::string text) {
	for (char &c : text) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	return text;
}

std::string FileName(const std::string &path) {
	size_t slash = path.find_last_of("\\/");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string Format(const char *format, ...) {
	char text[512];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	return text;
}

// ---- settings kept between games: cfg/p2practice/settings.cfg, run when the plugin loads ----
std::string SettingsPath() { return engine::GameDirectory() + "\\cfg\\p2practice\\settings.cfg"; }

// (to another file: a saved set of settings, cfg/p2practice/profiles/<name>.cfg)
void SaveSettings(const std::string &path = SettingsPath()) {
	const trainer::Settings &s = trainer::CurrentSettings();
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::u8path(path).parent_path(), ec);
	std::ofstream out(std::filesystem::u8path(path), std::ios::trunc);
	static const char *const resets[] = {"auto", "soft", "reload"};
	static const char *const styles[] = {"solid", "wire", "wirelow", "basic"};
	out << "// P2 Practice: the in-game menu's settings (written by the menu; run when the plugin loads)\n";
	out << Format("prac_set leadin %g\n", s.leadIn) << Format("prac_set ghost %d\n", s.ghost ? 1 : 0)
	    << Format("prac_set ghoststyle %s\n", styles[std::clamp(s.ghostStyle, 0, 3)]);
	if (s.ghostColorSet) out << Format("prac_set ghostcolor \"%d %d %d\"\n", s.ghostColor[0], s.ghostColor[1], s.ghostColor[2]);
	else out << "prac_set ghostcolor team\n";
	out << Format("prac_set ghostopacity %d\n", s.ghostOpacity) << Format("prac_set hud %d\n", s.hud ? 1 : 0)
	    << Format("prac_set speedometer %d\n", s.speedometer ? 1 : 0) << Format("prac_set strafe %d\n", s.strafe ? 1 : 0)
	    << Format("prac_set path %d\n", s.path ? 1 : 0)
	    << Format("prac_set loop %d\n", s.loop ? 1 : 0) << Format("prac_set autoreset %g\n", s.autoReset) << Format("prac_set speed %g\n", s.speed)
	    << Format("prac_set reset %s\n", resets[std::clamp(s.reset, 0, 2)]) << Format("prac_set challenge %d\n", s.challenge ? 1 : 0)
	    << Format("prac_set snapshots %d\n", s.snapshots ? 1 : 0) << Format("prac_set partner %d\n", s.partner ? 1 : 0)
	    << Format("prac_role %s\n", s.role == route::kTeamOrange ? "orange" : s.role == route::kTeamBlue ? "blue" : "auto")
	    << Format("prac_menukey %s\n", kMenuKeys[g.menuKey].name);
}

// ---- saved settings: cfg/p2practice/profiles/<name>.cfg ----
std::string ProfilesFolder() { return engine::GameDirectory() + "\\cfg\\p2practice\\profiles"; }

// A name fit for a file and for exec: letters, digits, - and _ (spaces become _)
std::string ProfileFileName(const std::string &name) {
	std::string out;
	for (char c : name) {
		if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') out += c;
		else if (c == ' ' && !out.empty() && out.back() != '_') out += '_';
	}
	while (!out.empty() && out.back() == '_') out.pop_back();
	return out.substr(0, 40);
}

std::vector<std::string> Profiles() {
	std::vector<std::string> names;
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(std::filesystem::u8path(ProfilesFolder()), ec))
		if (entry.path().extension() == ".cfg") names.push_back(entry.path().stem().u8string());
	std::sort(names.begin(), names.end());
	return names;
}

constexpr int kSaveAfterDrag = 30;  // frames a slider stays put before its value is written (see saveSettingsIn)

void Set(const std::string &line) {
	engine::Execute("%s", line.c_str());
	SaveSettings();
}

// ---- the boards page's chamber ----
void SelectBoard(const std::string &map) {
	if (map == g.boardMap) return;
	g.boardMap = map;
	g.selectedEntry = -1;
	g.boardScroll = 0;
	g.downloaded.clear();
	g.boardNote.clear();
	g.scrollToChamber = true;
	boards::Fetch(map);
}

void ShowBoard(const std::string &map) {
	g.page = Page::Boards;
	SelectBoard(map);
}

// ---- opening and closing ----
void Open(bool open) {
	g.open = open;
	gfx::SetCapture(open);
	if (!open) ui::Unfocus();
}

// prac_menu [practice|recent|runs|boards|settings|close]
void CmdMenu(const sdk::CCommand &args) {
	if (args.ArgC() < 2) {
		Open(!g.open);
		return;
	}
	std::string page = Lower(args.Arg(1));
	if (page == "close") return Open(false);
	if (page == "runs") g.page = Page::Runs;
	else if (page == "recent") g.page = Page::Recent;
	else if (page == "boards") {
		g.page = Page::Boards;
		// prac_menu boards <map>: that chamber's board
		if (args.ArgC() >= 3 && boards::OnBoard(args.Arg(2))) SelectBoard(args.Arg(2));
	}
	else if (page == "settings") g.page = Page::Settings;
	else g.page = Page::Practice;
	Open(true);
}

void CmdMenuKey(const sdk::CCommand &args) {
	if (args.ArgC() < 2) {
		engine::Print("prac_menukey <key> - the key that opens P2 Practice's menu (now: %s): INSERT, HOME, RSHIFT, F1-F12\n", kMenuKeys[g.menuKey].name);
		return;
	}
	for (int i = 0; i < static_cast<int>(sizeof(kMenuKeys) / sizeof(kMenuKeys[0])); i++)
		if (!_stricmp(args.Arg(1), kMenuKeys[i].name)) g.menuKey = i;
}

void CmdFolderAdd(const sdk::CCommand &args) {
	if (args.ArgC() < 2) {
		engine::Print("prac_folder_add <folder> - a folder of demos for the menu's RUNS page\n");
		return;
	}
	std::string path = args.ArgS();
	if (library::AddFolder(path)) engine::Print("Added %s.\n", path.c_str());
	else engine::Warn("%s doesn't exist or is listed already.\n", path.c_str());
}

void CmdFolders(const sdk::CCommand &) {
	const std::vector<std::string> &folders = library::Folders();
	for (size_t i = 0; i < folders.size(); i++) engine::Print("  %s\n", folders[i].c_str());
}

// ---- starting a session ----
// Whether a map's name, which comes out of a demo file, can go into a console command: only the characters map
// names are made of (a ; or a quote in it would run whatever follows as a command)
bool PlainMapName(const std::string &map) {
	if (map.empty()) return false;
	for (char c : map)
		if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '/' && c != '.') return false;
	return true;
}

void StartSession(const trainer::Status &s) {
	if (s.coop) {
		engine::Execute("prac_role %s", g.role == 1 ? "orange" : "blue");
		engine::Execute("prac_set partner %d", g.partner == 0 ? 1 : 0);
	}
	SaveSettings();
	bool watch = g.mode == 1;
	if (s.mapLoaded) {
		engine::CallCommand("prac_goto", Format("%.3f%s", g.takeOver, watch ? " watch" : "").c_str());
	} else {
		bool loadsMap = !s.coop || g.partner == 0;
		if (loadsMap && !PlainMapName(s.map)) {
			Note("This demo's map has a name the menu won't load (" + s.map + "): load the map yourself, then START.", 10);
			return;
		}
		engine::Execute("prac_stop");
		engine::CallCommand("prac_auto", Format("%sat %.3f", watch ? "watch " : "", g.takeOver).c_str());
		if (!s.coop) engine::Execute("map %s", s.map.c_str());
		else if (g.partner == 0) engine::Execute("ss_map %s", s.map.c_str());
		else {
			Note("Host " + s.chamber + " with your partner as usual: practice starts once you're both in.", 10);
			return;
		}
	}
	Open(false);
}

void LoadRun(const std::string &arguments, const std::string &what) {
	engine::CallCommand("prac_load", arguments.c_str());
	trainer::Status s = trainer::CurrentStatus();
	if (s.loaded && s.files.size() && arguments.find(s.files[0]) != std::string::npos) {
		g.page = Page::Practice;
		g.loadedKey.clear();  // (the practice page sets its choices for the new run)
		Note("Loaded " + what + ".");
	} else
		Note("That demo couldn't be loaded (see the console).", 8);
}

// A run from the RECENT page: its demos loaded again, and for a saved run how it was practiced
void LoadEntry(const recents::Entry &e) {
	std::string arguments;
	for (const std::string &f : e.files) arguments += (arguments.empty() ? "\"" : " \"") + f + "\"";
	if (e.saved) {
		g.pendingChoices = e;
		g.choicesPending = true;
	}
	LoadRun(arguments, e.saved ? e.name : e.chamber);
	if (g.page != Page::Practice) g.choicesPending = false;  // (it didn't load)
}

// Keeps the RECENT page's list up to date: a newly loaded run goes on top, and its session's attempts and best
// finish go with it
void TrackRecents(const trainer::Status &s) {
	if (!s.loaded || s.files.empty()) return;
	if (s.files[0] != g.recentKey) {
		g.recentKey = s.files[0];
		recents::Entry e;
		e.chamber = s.chamber;
		e.map = s.map;
		e.recordTime = s.recordTime;
		e.files = s.files;
		recents::Touch(e);
	}
	bool hasBest = !s.results.empty();
	float best = 0;
	for (const trainer::Result &r : s.results)
		if (&r == &s.results.front() || r.gap < best) best = r.gap;
	recents::UpdateSession(s.files, s.attempts, hasBest, best);
}

// ---- drawing ----
void Header(Rect window) {
	Rect bar{window.x, window.y, window.w, Px(58)};
	gfx::FillRect(bar.x, bar.y, bar.w, bar.h, theme::kHeader);
	int x = bar.x + Px(22);
	// the mark: two portal rings
	int r = Px(9), cy = bar.y + bar.h / 2;
	gfx::FillRect(x, cy - r, Px(6), 2 * r, theme::kBlue);
	gfx::FillRect(x + Px(9), cy - r, Px(6), 2 * r, theme::kOrange);
	x += Px(28);
	x += gfx::Text(x, cy - gfx::FontHeight(Font::Title) / 2, Font::Title, theme::kText, "P2 PRACTICE") + Px(30);
	struct Tab {
		Page page;
		const char *name;
	};
	const Tab tabs[] = {{Page::Practice, "PRACTICE"}, {Page::Recent, "RECENT"}, {Page::Runs, "RUNS"}, {Page::Boards, "BOARDS"}, {Page::Settings, "SETTINGS"}};
	for (const Tab &tab : tabs) {
		int w = gfx::TextWidth(Font::Bold, tab.name) + Px(28);
		Rect t{x, bar.y, w, bar.h};
		ui::Spot(tab.name, t);
		bool on = g.page == tab.page;
		bool hover = ui::Hovered(t);
		if (hover && !on) gfx::FillRect(t.x, t.y, t.w, t.h, theme::kField);
		ui::TextCentered(t, Font::Bold, on ? theme::kText : hover ? Color(210, 214, 222) : theme::kDim, tab.name);
		if (on) gfx::FillRect(t.x + Px(10), t.y + t.h - Px(4), t.w - Px(20), Px(4), theme::kBlue);
		if (ui::Clicked(t)) {
			g.page = tab.page;
			ui::Unfocus();
		}
		x += w;
	}
	std::string hint = std::string("TAB  next page  \xC2\xB7  ") + kMenuKeys[g.menuKey].name + " / ESC  close";
	ui::TextRight(bar.x + bar.w - Px(20), cy - gfx::FontHeight(Font::Small) / 2, Font::Small, theme::kFaint, hint);
}

void Footer(Rect window, const trainer::Status &s) {
	Rect bar{window.x, window.y + window.h - Px(34), window.w, Px(34)};
	gfx::FillRect(bar.x, bar.y, bar.w, bar.h, theme::kHeader);
	int y = bar.y + (bar.h - gfx::FontHeight(Font::Small)) / 2;
	std::string left;
	Color leftColor = theme::kDim;
	if (!ui::HoverHint().empty()) {
		left = ui::HoverHint();
		leftColor = Color(205, 210, 220);
	} else if (!g.note.empty() && GetTickCount() < g.noteUntil) left = g.note;
	else if (s.active) left = s.chamber + "  \xC2\xB7  " + s.mode + (s.attempts ? Format("  \xC2\xB7  attempt %d", s.attempts) : "");
	else if (s.loaded) left = s.chamber + " loaded" + (s.mapLoaded ? "" : "  \xC2\xB7  its map isn't up: START loads it");
	else left = "No run loaded: pick one in RUNS or BOARDS";
	ui::TextIn(bar.x + Px(20), y, bar.w - Px(300), Font::Small, leftColor, left);
	std::string right = std::string("save states ") + (s.snapshotsReady ? "ready" : "off") + "  \xC2\xB7  v" P2PRACTICE_VERSION;
	ui::TextRight(bar.x + bar.w - Px(20), y, Font::Small, theme::kFaint, right);
}

std::string GapText(float gap) {
	// (hundredths, as the game's clock; "-0.00" reads as a tie)
	int hundredths = static_cast<int>(std::lround(gap * 100));
	if (hundredths == 0) return "\xC2\xB1" "0.00";
	return Format("%c%d.%02d", hundredths < 0 ? '-' : '+', std::abs(hundredths) / 100, std::abs(hundredths) % 100);
}

// The session so far, under the practice controls: the attempts' finishes, newest first, and the best
void SessionPanel(const trainer::Status &s) {
	ui::Space(Px(6));
	Rect area = ui::Area();
	if (area.h < Px(80)) return;
	ui::Panel(area, theme::kHeader, theme::kLine);
	int pad = Px(14), x = area.x + pad, w = area.w - 2 * pad, y = area.y + Px(10);
	const std::vector<trainer::Result> &results = s.results;
	gfx::Text(x, y, Font::Small, theme::kFaint, "THIS SESSION");
	ui::TextRight(x + w, y, Font::Small, theme::kDim,
	              Format("%d attempt%s  \xC2\xB7  %d finished", s.attempts, s.attempts == 1 ? "" : "s", static_cast<int>(results.size())));
	y += gfx::FontHeight(Font::Small) + Px(10);
	if (results.empty()) {
		ui::TextIn(x, y, w, Font::Small, theme::kDim, s.attempts ? "Reach the finish and the time shows here." : "Start an attempt: its finish time shows here.");
		return;
	}
	// the best of the session, next to the record
	const trainer::Result *best = &results[0];
	for (const trainer::Result &r : results)
		if (r.gap < best->gap) best = &r;
	int bw = gfx::Text(x, y, Font::Title, best->gap <= 0 ? theme::kGreen : theme::kText, route::FormatTime(best->seconds).c_str());
	int cy = y + (gfx::FontHeight(Font::Title) - gfx::FontHeight(Font::Normal)) / 2;
	int gw = gfx::Text(x + bw + Px(12), cy, Font::Bold, best->gap <= 0 ? theme::kGreen : theme::kOrange, GapText(best->gap).c_str());
	ui::TextIn(x + bw + gw + Px(24), cy, w - bw - gw - Px(24), Font::Small, theme::kDim,
	           Format("best, from part %d  \xC2\xB7  record %s", best->part + 1, s.recordTime.c_str()));
	y += gfx::FontHeight(Font::Title) + Px(8);
	// the best from each part (marked where a part has been finished from more than once)
	std::map<int, int> partBest, partCount;
	for (size_t i = 0; i < results.size(); i++) {
		int part = results[i].part;
		partCount[part]++;
		if (!partBest.count(part) || results[i].seconds < results[partBest[part]].seconds) partBest[part] = static_cast<int>(i);
	}
	// the latest attempts, as many as fit
	int rowH = gfx::FontHeight(Font::Normal) + Px(6);
	int rows = (area.y + area.h - Px(8) - y) / rowH;
	for (int i = 0; i < rows && i < static_cast<int>(results.size()); i++) {
		size_t index = results.size() - 1 - i;
		const trainer::Result &r = results[index];
		if (i % 2) gfx::FillRect(area.x + Px(4), y - Px(3), area.w - Px(8), rowH, theme::kZebra);
		gfx::Text(x, y, Font::Small, theme::kFaint, Format("#%d", static_cast<int>(index) + 1).c_str());
		gfx::Text(x + Px(52), y, Font::Small, theme::kDim, Format("part %02d", r.part + 1).c_str());
		gfx::Text(x + Px(140), y, Font::Bold, theme::kText, route::FormatTime(r.seconds).c_str());
		gfx::Text(x + Px(230), y, Font::Normal, r.gap <= 0 ? theme::kGreen : theme::kOrange, GapText(r.gap).c_str());
		bool partsBest = partCount[r.part] > 1 && partBest[r.part] == static_cast<int>(index);
		std::string tags = std::string(r.flags ? "flags" : "") + (partsBest ? (r.flags ? "  \xC2\xB7  best from this part" : "best from this part") : "");
		ui::TextRight(x + w, y, Font::Small, partsBest ? theme::kBlue : theme::kFaint, tags);
		y += rowH;
	}
}

// The practice page: the loaded run, its parts, and how to practice it
void PracticePage(Rect area, const trainer::Status &s) {
	if (!s.loaded) {
		ui::BeginArea(area.Inset(Px(40)));
		ui::Label("No run loaded", Font::Title);
		ui::Space(Px(6));
		ui::Label("Pick a demo of yours in RUNS, or fetch one from the leaderboards in BOARDS.", Font::Normal, theme::kDim);
		ui::Space(Px(20));
		ui::Rect r = ui::TakeWidth(Px(220), Px(40));
		if (ui::ButtonIn(r, "BROWSE RUNS", true)) g.page = Page::Runs;
		ui::SameLine();
		if (ui::ButtonIn(ui::TakeWidth(Px(220), Px(40)), "BOARDS")) g.page = Page::Boards;
		// in a chamber of the boards: its board straight away
		std::string map = engine::MapName();
		if (gfx::InGame() && boards::OnBoard(map)) {
			ui::SameLine();
			std::string label = "BOARD FOR " + maps::ChamberName(map);
			if (ui::ButtonIn(ui::TakeWidth(gfx::TextWidth(Font::Normal, label.c_str()) + Px(40), Px(40)), label)) ShowBoard(map);
		}
		ui::EndArea();
		return;
	}
	// choices set fresh for each newly loaded run
	std::string key = s.files.empty() ? s.map : s.files[0];
	if (key != g.loadedKey) {
		g.loadedKey = key;
		// (from the part the trainer is on, when it is one of this run's)
		size_t part = s.part >= 0 && static_cast<size_t>(s.part) < s.parts.size() ? static_cast<size_t>(s.part) : 0;
		g.takeOver = s.parts.empty() ? 0 : s.parts[part].start;
		const trainer::Settings &set = trainer::CurrentSettings();
		g.role = set.role == route::kTeamOrange ? 1 : 0;
		g.partner = set.partner ? 0 : 1;
		g.partsScroll = 0;
		g.saveName.clear();
		// a saved run: how it was practiced
		if (g.choicesPending) {
			g.choicesPending = false;
			const recents::Entry &e = g.pendingChoices;
			g.takeOver = std::clamp(e.takeOver, 0.0f, s.runSeconds);
			g.mode = e.mode;
			g.role = e.role;
			g.partner = e.partner;
			Set(Format("prac_set leadin %g", e.leadIn));
			Set(Format("prac_set speed %g", e.speed));
			g.saveName = e.name;
		}
	}

	int pad = Px(26);
	Rect left{area.x + pad, area.y + pad, area.w * 52 / 100 - pad, area.h - 2 * pad};
	Rect right{left.x + left.w + Px(30), area.y + pad, area.x + area.w - pad - (left.x + left.w + Px(30)), area.h - 2 * pad};

	// --- left: the run and its parts ---
	int tw = gfx::Text(left.x, left.y - Px(10), Font::Huge, theme::kText, s.recordTime.c_str());
	int tx = left.x + tw + Px(22);
	ui::TextIn(tx, left.y + Px(8), left.x + left.w - tx, Font::Title, theme::kText, s.chamber);
	std::string sub = s.map + (s.coop ? "  \xC2\xB7  co-op" : "  \xC2\xB7  single player");
	ui::TextIn(tx, left.y + Px(8) + gfx::FontHeight(Font::Title), left.x + left.w - tx, Font::Small, theme::kDim, sub);
	int y = left.y + gfx::FontHeight(Font::Huge);
	std::string files;
	for (const std::string &f : s.files) files += (files.empty() ? "" : "  +  ") + FileName(f);
	ui::TextIn(left.x, y, left.w, Font::Small, theme::kFaint, files);
	y += gfx::FontHeight(Font::Small) + Px(16);

	// a bar of the run with its parts and where the take-over is
	Rect bar{left.x, y, left.w, Px(14)};
	gfx::FillRect(bar.x, bar.y, bar.w, bar.h, theme::kField);
	float total = std::max(0.01f, s.runSeconds);
	for (size_t i = 0; i < s.parts.size(); i++) {
		int x0 = bar.x + static_cast<int>(bar.w * std::clamp(s.parts[i].start / total, 0.0f, 1.0f));
		gfx::FillRect(x0, bar.y, Px(2), bar.h, theme::kHeader);
	}
	int mark = bar.x + static_cast<int>(bar.w * std::clamp(g.takeOver / total, 0.0f, 1.0f));
	gfx::FillRect(bar.x, bar.y + Px(4), mark - bar.x, bar.h - Px(8), g.mode == 1 ? theme::kDim : theme::kBlue);
	gfx::FillRect(mark - Px(2), bar.y - Px(4), Px(4), bar.h + Px(8), theme::kText);
	Rect barGrab{bar.x, bar.y - Px(6), bar.w, bar.h + Px(12)};
	ui::HintFor(barGrab, "Click or drag to choose where you take over from the run");
	if (ui::Hovered(barGrab)) {
		float at = total * std::clamp(static_cast<float>(gfx::MouseX() - bar.x) / bar.w, 0.0f, 1.0f);
		if (gfx::KeyDown(gfx::kMouseLeft)) g.takeOver = at;
		// where the mouse is: the time and the part
		std::string partName;
		for (const trainer::Part &part : s.parts)
			if (at >= part.start - 0.001f) partName = part.name;
		ui::Tooltip(route::FormatTime(at) + (partName.empty() ? "" : "  \xC2\xB7  " + partName));
	}
	y += bar.h + Px(18);

	gfx::Text(left.x, y, Font::Small, theme::kFaint, "START FROM");
	y += gfx::FontHeight(Font::Small) + Px(6);
	Rect listArea{left.x, y, left.w, left.y + left.h - y};
	int rowH = Px(40);
	// the session's best finish from each part, shown on its row
	std::map<int, float> bestFrom;
	for (const trainer::Result &r : s.results)
		if (!bestFrom.count(r.part) || r.gap < bestFrom[r.part]) bestFrom[r.part] = r.gap;
	ui::List list = ui::BeginList(listArea, rowH, static_cast<int>(s.parts.size()), g.partsScroll);
	for (int i = list.first; i < list.last; i++) {
		bool clicked;
		bool selected = g.takeOver >= s.parts[i].start - 0.001f && (i + 1 >= static_cast<int>(s.parts.size()) || g.takeOver < s.parts[i + 1].start - 0.001f);
		Rect row = ui::ListRow(list, i, selected, clicked);
		if (row.y < -1000) continue;
		ui::Spot("part:" + std::to_string(i), row);
		Rect num{row.x + Px(10), row.y + Px(8), Px(34), row.h - Px(16)};
		gfx::FillRect(num.x, num.y, num.w, num.h, selected ? theme::kBlue : theme::kField);
		ui::TextCentered(num, Font::Bold, selected ? Color(10, 14, 20) : theme::kDim, Format("%02d", i + 1));
		int ty = row.y + (row.h - gfx::FontHeight(Font::Normal)) / 2;
		std::string times = route::FormatTime(s.parts[i].start) + " - " + route::FormatTime(s.parts[i].end);
		int timesW = ui::TextRight(row.x + row.w - Px(12), ty, Font::Small, theme::kDim, times);
		auto best = bestFrom.find(i);
		if (best != bestFrom.end()) {
			// (a little gap, then the best against the record: green when at or under it)
			timesW += Px(14) + ui::TextRight(row.x + row.w - Px(12) - timesW - Px(14), ty, Font::Small,
			                                 best->second <= 0 ? theme::kGreen : theme::kOrange, GapText(best->second));
		}
		ui::TextIn(num.x + num.w + Px(14), ty, row.w - num.w - timesW - Px(50), Font::Normal, theme::kText, s.parts[i].name);
		if (s.active && s.part == i) gfx::FillRect(row.x + row.w - Px(4), row.y, Px(4), row.h, theme::kOrange);
		if (clicked) g.takeOver = s.parts[i].start;
	}
	ui::EndList(list);
	ui::HintFor(listArea, "Click a part to take over at its start; the coloured time is your best finish from it against the record");

	// --- right: how to practice it ---
	ui::BeginArea(right);
	ui::Choice("MODE", g.mode, {"Practice", "Watch the run"});
	ui::Hint("Practice: the run plays up to the take-over, then you play on. Watch: the whole run plays by itself");
	if (s.coop) {
		ui::Choice("PLAY AS", g.role, {"Blue (ATLAS)", "Orange (P-body)"}, {theme::kBlue, theme::kOrange});
		ui::Hint("The robot you play; the other one follows the run");
		ui::Choice("PARTNER", g.partner, {"Bot", "Real partner"});
		ui::Hint("Bot: the other robot is played from the run. Real partner: someone joins you in split screen or online");
	}
	ui::Slider("TAKE OVER AT", g.takeOver, 0, total, "%.2f s", true);
	ui::Hint("The moment of the run you take over at: drag, or click the number to type one");
	const trainer::Settings &set = trainer::CurrentSettings();
	float leadIn = set.leadIn, speed = set.speed;
	if (ui::Stepper("LEAD-IN", leadIn, 0.5f, 0, 10, "%.1f s")) Set(Format("prac_set leadin %g", leadIn));
	ui::Hint("How much of the run plays before you take over, to get into its rhythm");
	if (ui::Stepper("GAME SPEED", speed, 0.05f, 0.1f, 2, "%.2fx")) Set(Format("prac_set speed %g", speed));
	ui::Hint("Slow the game down to learn a hard part (1.00x for real practice)");
	bool loop = set.loop;
	if (ui::Toggle("Start again by itself after the finish", loop)) Set(Format("prac_set loop %d", loop ? 1 : 0));
	ui::Space(Px(10));
	std::string start = g.mode == 1 ? "WATCH" : "START";
	if (!s.mapLoaded) start += s.coop && g.partner == 1 ? "  (host the map)" : "  (loads " + s.chamber + ")";
	Rect startButton = ui::Take(Px(52));
	ui::Spot("start", startButton);
	if (ui::ButtonIn(startButton, start, true)) StartSession(s);
	if (s.active) {
		int third = (ui::AreaWidth() - 2 * Px(8)) / 3;
		if (ui::ButtonIn(ui::TakeWidth(third, Px(38)), "RETRY")) {
			engine::Execute("prac_retry");
			Open(false);
		}
		ui::Hint("Start the attempt again from the same moment (prac_retry: bind it to a key)");
		ui::SameLine();
		if (ui::ButtonIn(ui::TakeWidth(third, Px(38)), "NEXT PART")) {
			engine::Execute("prac_next");
			Open(false);
		}
		ui::Hint("Practice the part after this one (prac_next)");
		ui::SameLine();
		if (ui::ButtonIn(ui::TakeWidth(third, Px(38)), "STOP")) engine::Execute("prac_stop");
		ui::Hint("End practice: the level is yours again (prac_stop)");
	}
	// keeping this run with these choices, for the RECENT page
	Rect saveRow = ui::Take(Px(34));
	ui::BeginArea({saveRow.x, saveRow.y, saveRow.w - Px(130), saveRow.h});
	bool entered = ui::TextBox("savename", g.saveName, "Name to save this setup under (" + s.chamber + Format(" from %.2f s)", g.takeOver));
	ui::Hint("Type a name, then SAVE: the run, where you take over and how, kept on the RECENT page");
	ui::EndArea();
	Rect saveButton{saveRow.x + saveRow.w - Px(120), saveRow.y, Px(120), saveRow.h};
	if (ui::ButtonIn(saveButton, "SAVE") || entered) {
		recents::Entry e;
		e.name = g.saveName.empty() ? s.chamber + Format(" from %.2f", g.takeOver) : g.saveName;
		e.chamber = s.chamber;
		e.map = s.map;
		e.recordTime = s.recordTime;
		e.files = s.files;
		e.takeOver = g.takeOver;
		e.mode = g.mode;
		e.role = g.role;
		e.partner = g.partner;
		e.leadIn = set.leadIn;
		e.speed = set.speed;
		recents::Save(e);
		g.saveName = e.name;
		ui::Unfocus();
		Note("Saved \"" + e.name + "\": it is on the RECENT page.");
	}
	ui::HintFor(saveButton, "Keep this run with where you take over, the mode, robot, partner, lead-in and speed");
	SessionPanel(s);
	ui::EndArea();
}

// The runs page: every demo in the player's folders
void RunsPage(Rect area) {
	const std::vector<library::Run> &runs = library::Runs();
	int pad = Px(22);
	ui::BeginArea({area.x + pad, area.y + pad, area.w - 2 * pad, area.h - 2 * pad});
	// the level that is up, for THIS MAP
	std::string level = gfx::InGame() ? engine::MapName() : "";
	bool canFilter = !level.empty();
	if (!canFilter) g.runsThisMap = false;
	Rect top = ui::Take(Px(36));
	ui::BeginArea({top.x, top.y, top.w - Px(480), top.h});
	ui::TextBox("search", g.search, "Search: chamber, player, file...");
	ui::Hint("Type part of a chamber's name, a player or a file name; Ctrl+Backspace clears it");
	ui::EndArea();
	Rect thisMap{top.x + top.w - Px(470), top.y, Px(150), top.h};
	if (ui::ButtonIn(thisMap, g.runsThisMap ? "ALL MAPS" : "THIS MAP", g.runsThisMap, canFilter)) g.runsThisMap = !g.runsThisMap;
	ui::HintFor(thisMap, canFilter ? (g.runsThisMap ? "Show the runs of every chamber again" : "Show only the runs of " + maps::ChamberName(level))
	                               : "Load a chamber to see only its runs");
	if (ui::ButtonIn({top.x + top.w - Px(310), top.y, Px(150), top.h}, g.foldersOpen ? "HIDE FOLDERS" : "FOLDERS")) g.foldersOpen = !g.foldersOpen;
	ui::HintFor({top.x + top.w - Px(310), top.y, Px(150), top.h}, "The folders the demos are read from: add your own");
	if (ui::ButtonIn({top.x + top.w - Px(150), top.y, Px(150), top.h}, "REFRESH")) library::Refresh();
	ui::HintFor({top.x + top.w - Px(150), top.y, Px(150), top.h}, "Read the folders again (new demos show up by themselves on the next open)");

	if (g.foldersOpen) {
		const std::vector<std::string> &folders = library::Folders();
		for (size_t i = 0; i < folders.size(); i++) {
			Rect r = ui::Take(Px(30));
			ui::TextIn(r.x + Px(6), r.y + (r.h - gfx::FontHeight(Font::Small)) / 2, r.w - Px(140), Font::Small, theme::kText, folders[i]);
			if (ui::ButtonIn({r.x + r.w - Px(120), r.y, Px(120), r.h}, "REMOVE")) {
				library::RemoveFolder(i);
				break;
			}
		}
		Rect add = ui::Take(Px(36));
		ui::BeginArea({add.x, add.y, add.w - Px(160), add.h});
		bool enter = ui::TextBox("folder", g.newFolder, "Paste a folder of demos (Ctrl+V) and press Enter");
		ui::EndArea();
		if (ui::ButtonIn({add.x + add.w - Px(150), add.y, Px(150), add.h}, "ADD FOLDER") || enter) {
			g.folderNote = library::AddFolder(g.newFolder) ? "" : "That folder doesn't exist or is listed already.";
			if (g.folderNote.empty()) g.newFolder.clear();
		}
		if (!g.folderNote.empty()) ui::Label(g.folderNote, Font::Small, theme::kOrange);
		ui::Space(Px(6));
	}

	// the runs matching the search, the map and the order (worked out again only when one of them or the runs change)
	std::string shownKey = g.search + '\x01' + (g.runsThisMap ? level : "") + '\x01' + std::to_string(g.runsSort);
	if (g.searchShown != shownKey || g.libraryVersion != library::Version()) {
		g.searchShown = shownKey;
		g.libraryVersion = library::Version();
		g.shown.clear();
		std::string needle = Lower(g.search);
		for (size_t i = 0; i < runs.size(); i++) {
			if (g.runsThisMap && runs[i].demo->map != level) continue;
			if (needle.empty() || runs[i].search.find(needle) != std::string::npos) g.shown.push_back(static_cast<int>(i));
		}
		if (g.runsSort == 1)
			std::stable_sort(g.shown.begin(), g.shown.end(), [&](int a, int b) {
				// (demos without a time last)
				float ta = runs[a].demo->hasTime ? runs[a].demo->seconds : 1e9f, tb = runs[b].demo->hasTime ? runs[b].demo->seconds : 1e9f;
				return ta < tb;
			});
		else if (g.runsSort == 2)
			std::stable_sort(g.shown.begin(), g.shown.end(), [&](int a, int b) { return runs[a].demo->modified > runs[b].demo->modified; });
	}

	// columns (CHAMBER, TIME and FILE sort the list: chamber order, fastest, newest)
	Rect head = ui::Take(Px(26));
	int w = head.w - Px(12);
	int cx[] = {head.x + Px(12), head.x + w * 30 / 100, head.x + w * 41 / 100, head.x + w * 70 / 100, head.x + w * 82 / 100};
	const char *names[] = {"CHAMBER", "TIME", "PLAYERS", "MODE", "FILE"};
	const int sortOf[] = {0, 1, -1, -1, 2};
	for (int c = 0; c < 5; c++) {
		Rect cell{cx[c] - Px(4), head.y, (c + 1 < 5 ? cx[c + 1] : head.x + head.w) - cx[c], head.h};
		bool sorts = sortOf[c] >= 0, on = sorts && g.runsSort == sortOf[c];
		bool hover = sorts && ui::Hovered(cell);
		std::string name = std::string(names[c]) + (on && c == 1 ? "  (fastest)" : on && c == 4 ? "  (newest)" : "");
		int tw = gfx::Text(cx[c], head.y + Px(4), Font::Small, on ? theme::kText : hover ? theme::kDim : theme::kFaint, name.c_str());
		if (on) gfx::FillRect(cx[c], head.y + head.h - Px(3), tw, Px(2), theme::kBlue);
		if (sorts) {
			ui::Spot(std::string("sort:") + names[c], cell);
			ui::HintFor(cell, c == 0 ? "Sort by chamber, in the game's order" : c == 1 ? "Sort by time, fastest first" : "Sort by date, newest first");
			if (ui::Clicked(cell)) g.runsSort = sortOf[c];
		}
	}
	Rect listArea = ui::Area();
	listArea.h -= Px(52);
	int rowH = Px(34);
	ui::List list = ui::BeginList(listArea, rowH, static_cast<int>(g.shown.size()), g.runsScroll);
	for (int i = list.first; i < list.last; i++) {
		const library::Run &run = runs[g.shown[i]];
		bool clicked;
		Rect row = ui::ListRow(list, i, g.selectedRun == run.demo->path, clicked);
		if (row.y < -1000) continue;
		ui::Spot("run:" + std::to_string(i), row);
		int ty = row.y + (row.h - gfx::FontHeight(Font::Normal)) / 2;
		Color text = run.demo->ok ? theme::kText : theme::kFaint;
		ui::TextIn(cx[0], ty, cx[1] - cx[0] - Px(10), Font::Bold, text, run.chamber);
		ui::TextIn(cx[1], ty, cx[2] - cx[1] - Px(10), Font::Normal, text, run.timeText);
		ui::TextIn(cx[2], ty, cx[3] - cx[2] - Px(10), Font::Normal, text, run.players);
		if (run.demo->ok && run.demo->coop) {
			// the robots the demos give: blue (host), orange (client)
			bool blue = run.partner || run.demo->host, orange = run.partner || !run.demo->host;
			int sq = Px(10), sy = row.y + (row.h - sq) / 2;
			if (blue) gfx::FillRect(cx[3], sy, sq, sq, theme::kBlue);
			if (orange) gfx::FillRect(cx[3] + Px(14), sy, sq, sq, theme::kOrange);
		} else
			ui::TextIn(cx[3], ty, cx[4] - cx[3] - Px(10), Font::Normal, theme::kDim, run.demo->ok ? "SP" : "");
		ui::TextIn(cx[4], ty, row.x + row.w - cx[4] - Px(6), Font::Small, theme::kDim, run.files);
		if (clicked) {
			bool twice = g.lastClicked == run.demo->path && GetTickCount() - g.lastClick < 400;
			g.selectedRun = run.demo->path;
			g.lastClicked = run.demo->path;
			g.lastClick = GetTickCount();
			if (twice && run.demo->ok) LoadRun(library::LoadCommand(run), run.chamber);
		}
	}
	ui::EndList(list);
	ui::HintFor(listArea, "Double-click a run to load it");

	Rect bottom{listArea.x, listArea.y + listArea.h + Px(12), listArea.w, Px(40)};
	int done, total;
	library::Progress(done, total);
	std::string count = Format("%d runs", static_cast<int>(g.shown.size()));
	if (library::Busy()) count += Format("  \xC2\xB7  reading demos %d / %d", done, total);
	gfx::Text(bottom.x, bottom.y + (bottom.h - gfx::FontHeight(Font::Small)) / 2, Font::Small, theme::kDim, count.c_str());
	// the selected run, where it is in the list now (nowhere once its demo is gone)
	const library::Run *selected = nullptr;
	if (!g.selectedRun.empty())
		for (const library::Run &run : runs)
			if (run.demo->path == g.selectedRun) selected = &run;
	bool canLoad = selected && selected->demo->ok;
	if (ui::ButtonIn({bottom.x + bottom.w - Px(260), bottom.y, Px(260), bottom.h}, "LOAD THIS RUN", true, canLoad))
		LoadRun(library::LoadCommand(*selected), selected->chamber);
	ui::EndArea();
}

// Asks for a run's demo(s) from the board shown, remembering the chamber it is for
void GetDemo(const boards::Entry &entry) {
	boards::Download(g.boardMap, entry);
	g.downloading = true;
	g.downloadMap = g.boardMap;
	g.downloadName = maps::ChamberName(g.boardMap);
	g.boardNote.clear();
}

// A demo asked for from the boards is loaded as soon as it is here. Looked at every frame, whatever page is up
// (and with the menu closed): left to the boards page, it would be loaded only when the player came back to that
// page, over whatever was being practiced by then and under the name of the board shown then.
void FinishDownload() {
	if (!g.downloading) return;
	boards::State download = boards::DownloadState();
	if (download == boards::State::Busy) return;
	g.downloading = false;
	if (download == boards::State::Ready && !boards::DownloadedFiles().empty()) {
		std::string arguments;
		for (const std::string &f : boards::DownloadedFiles()) arguments += (arguments.empty() ? "\"" : " \"") + f + "\"";
		g.downloaded.clear();  // (the board shown marks it "saved" from now on)
		library::Refresh();
		LoadRun(arguments, g.downloadName);
	} else {
		std::string failed = "Download failed: " + boards::DownloadError();
		if (g.downloadMap == g.boardMap) g.boardNote = failed;
		Note(g.downloadName + ": " + failed, 8);
	}
}

// The boards page: chambers on the left, the chosen chamber's board on the right
void BoardsPage(Rect area) {
	int pad = Px(22);
	Rect left{area.x + pad, area.y + pad, Px(330), area.h - 2 * pad};
	Rect right{left.x + left.w + Px(22), left.y, area.x + area.w - pad - (left.x + left.w + Px(22)), left.h};

	// with no chamber chosen yet: the one being practiced, or the level that is up
	if (g.boardMap.empty()) {
		trainer::Status s = trainer::CurrentStatus();
		std::string level = gfx::InGame() ? engine::MapName() : "";
		if (s.loaded && boards::OnBoard(s.map)) SelectBoard(s.map);
		else if (boards::OnBoard(level)) SelectBoard(level);
	}

	// chambers by chapter, one list, cut down to those matching the search
	struct Line {
		bool chapter;
		std::string text, map, search;
	};
	static std::vector<Line> all;
	if (all.empty())
		for (const boards::Chapter &chapter : boards::Chapters()) {
			all.push_back({true, chapter.name, "", ""});
			for (const auto &chamber : chapter.chambers) all.push_back({false, chamber.second, chamber.first, Lower(chamber.second + " " + chamber.first + " " + chapter.name)});
		}
	ui::BeginArea({left.x, left.y, left.w - Px(12), Px(36)});
	ui::TextBox("chamberfilter", g.chamberFilter, "Find a chamber...");
	ui::Hint("Type part of a chamber's name, its map's name or its chapter");
	ui::EndArea();
	left.y += Px(44);
	left.h -= Px(44);
	std::vector<const Line *> lines;
	std::string needle = Lower(g.chamberFilter);
	for (size_t i = 0; i < all.size(); i++) {
		if (!all[i].chapter) continue;
		// a chapter: all of it when its name matches, else the chambers that do (and its heading over them)
		bool whole = needle.empty() || Lower(all[i].text).find(needle) != std::string::npos;
		bool heading = false;
		for (size_t j = i + 1; j < all.size() && !all[j].chapter; j++) {
			if (!whole && all[j].search.find(needle) == std::string::npos) continue;
			if (!heading) lines.push_back(&all[i]);
			heading = true;
			lines.push_back(&all[j]);
		}
	}
	int rowH = Px(30);
	if (g.scrollToChamber) {
		g.scrollToChamber = false;
		for (size_t i = 0; i < lines.size(); i++)
			if (lines[i]->map == g.boardMap) {
				// (its chapter's heading in view above it)
				g.chaptersScroll = std::max(0, static_cast<int>(i) * rowH - left.h / 3);
				ui::ScrollTo(g.chaptersScroll, static_cast<int>(i), rowH, left.h, static_cast<int>(lines.size()));
			}
	}
	ui::Spot("chambers", left);
	ui::List chambers = ui::BeginList(left, rowH, static_cast<int>(lines.size()), g.chaptersScroll);
	for (int i = chambers.first; i < chambers.last; i++) {
		const Line &line = *lines[i];
		bool clicked;
		if (line.chapter) {
			Rect row{left.x, left.y + i * rowH - g.chaptersScroll, left.w - Px(12), rowH};
			if (row.y >= left.y && row.y + row.h <= left.y + left.h)
				ui::TextIn(row.x + Px(4), row.y + row.h - gfx::FontHeight(Font::Small) - Px(2), row.w, Font::Small, theme::kFaint, line.text);
			continue;
		}
		Rect row = ui::ListRow(chambers, i, line.map == g.boardMap, clicked);
		if (row.y < -1000) continue;
		ui::Spot("chamber:" + line.text, row);
		ui::TextIn(row.x + Px(14), row.y + (row.h - gfx::FontHeight(Font::Normal)) / 2, row.w - Px(20), Font::Normal, theme::kText, line.text);
		if (clicked && line.map != g.boardMap) {
			SelectBoard(line.map);
			g.scrollToChamber = false;  // (clicked: it is in view)
		}
	}
	ui::EndList(chambers);
	if (lines.empty()) ui::TextIn(left.x + Px(4), left.y + Px(8), left.w, Font::Small, theme::kDim, "No chamber matches.");

	boards::State state = boards::BoardState();
	ui::BeginArea(right);
	if (g.boardMap.empty()) {
		ui::Label("Leaderboards", Font::Title);
		ui::Label("Pick a chamber: every run on its board.portal2.sr board is listed, and its demo is a click away.", Font::Normal, theme::kDim);
		ui::EndArea();
		return;
	}
	ui::Label(maps::ChamberName(g.boardMap), Font::Title);
	// (a board for another chamber, still on screen while this one's comes, is never shown under this name)
	if (state == boards::State::Busy || boards::BoardMap() != g.boardMap) {
		ui::Label("Fetching the board...", Font::Normal, theme::kDim);
		ui::EndArea();
		return;
	}
	if (state == boards::State::Failed) {
		ui::Label("Couldn't get the board: " + boards::BoardError(), Font::Normal, theme::kOrange);
		if (ui::Button("TRY AGAIN")) boards::Fetch(g.boardMap);
		ui::EndArea();
		return;
	}
	const std::vector<boards::Entry> &entries = boards::Entries();
	if (g.downloaded.empty())
		for (const boards::Entry &e : entries) g.downloaded[e.changelogId] = boards::Downloaded(g.boardMap, e);

	Rect head = ui::Take(Px(24));
	int w = head.w - Px(12);
	int cx[] = {head.x + Px(10), head.x + w * 8 / 100, head.x + w * 22 / 100, head.x + w * 74 / 100, head.x + w * 88 / 100};
	const char *names[] = {"#", "TIME", "PLAYERS", "DATE", "DEMO"};
	for (int c = 0; c < 5; c++) gfx::Text(cx[c], head.y + Px(2), Font::Small, theme::kFaint, names[c]);
	Rect listArea = ui::Area();
	listArea.h -= Px(52);
	int entryH = Px(34);
	ui::List list = ui::BeginList(listArea, entryH, static_cast<int>(entries.size()), g.boardScroll);
	for (int i = list.first; i < list.last; i++) {
		const boards::Entry &e = entries[i];
		bool clicked;
		Rect row = ui::ListRow(list, i, g.selectedEntry == i, clicked);
		if (row.y < -1000) continue;
		ui::Spot("entry:" + std::to_string(i), row);
		int ty = row.y + (row.h - gfx::FontHeight(Font::Normal)) / 2;
		gfx::Text(cx[0], ty, Font::Normal, theme::kDim, std::to_string(e.rank).c_str());
		gfx::Text(cx[1], ty, Font::Bold, theme::kText, boards::FormatScore(e.score).c_str());
		ui::TextIn(cx[2], ty, cx[3] - cx[2] - Px(10), Font::Normal, theme::kText, e.Players());
		gfx::Text(cx[3], ty, Font::Small, theme::kDim, e.date.c_str());
		bool have = g.downloaded[e.changelogId];
		gfx::Text(cx[4], ty, Font::Small, have ? theme::kGreen : e.Complete() ? theme::kDim : theme::kFaint, have ? "saved" : e.Complete() ? "yes" : "none");
		if (clicked) {
			bool twice = g.selectedEntry == i && GetTickCount() - g.lastClick < 400;
			g.selectedEntry = i;
			g.lastClick = GetTickCount();
			if (twice && e.Complete() && !g.downloading) GetDemo(e);
		}
	}
	ui::EndList(list);
	ui::HintFor(listArea, "Double-click a run to get its demo and load it (\"saved\": it is on this computer already)");

	// (a finished download is loaded by FinishDownload)
	Rect bottom{listArea.x, listArea.y + listArea.h + Px(12), listArea.w, Px(40)};
	bool canGet = g.selectedEntry >= 0 && g.selectedEntry < static_cast<int>(entries.size()) && entries[g.selectedEntry].Complete();
	std::string label = g.downloading ? "DOWNLOADING..." : canGet && g.downloaded[entries[g.selectedEntry].changelogId] ? "LOAD THIS RUN" : "GET DEMO";
	if (ui::ButtonIn({bottom.x + bottom.w - Px(260), bottom.y, Px(260), bottom.h}, label, true, canGet && !g.downloading))
		GetDemo(entries[g.selectedEntry]);
	std::string info = g.boardNote.empty() ? Format("%d runs on the board", static_cast<int>(entries.size())) : g.boardNote;
	ui::TextIn(bottom.x, bottom.y + (bottom.h - gfx::FontHeight(Font::Small)) / 2, bottom.w - Px(280), Font::Small,
	           g.boardNote.empty() ? theme::kDim : theme::kOrange, info);
	ui::EndArea();
}

// The recent page: the runs kept on purpose (with how they're practiced), then what was practiced lately
void RecentPage(Rect area) {
	const std::vector<recents::Entry> &saved = recents::Saved();
	const std::vector<recents::Entry> &recent = recents::Recent();
	int pad = Px(22);
	ui::BeginArea({area.x + pad, area.y + pad, area.w - 2 * pad, area.h - 2 * pad});
	int rowH = Px(34);
	auto gapText = [](const recents::Entry &e) { return e.hasBest ? GapText(e.bestGap) : std::string("-"); };

	// --- saved runs ---
	ui::Caption(Format("SAVED RUNS  (%d)", static_cast<int>(saved.size())));
	if (saved.empty()) {
		ui::Label("None yet: on the PRACTICE page, name a setup and press SAVE to keep it here.", Font::Small, theme::kDim);
	} else {
		Rect head = ui::Take(Px(22));
		int w = head.w - Px(12);
		int cx[] = {head.x + Px(12), head.x + w * 34 / 100, head.x + w * 58 / 100, head.x + w * 72 / 100, head.x + w * 86 / 100};
		const char *names[] = {"NAME", "CHAMBER", "TAKE OVER", "MODE", "SAVED"};
		for (int c = 0; c < 5; c++) gfx::Text(cx[c], head.y + Px(2), Font::Small, theme::kFaint, names[c]);
		int shownRows = std::min(static_cast<int>(saved.size()), 5);
		Rect listArea = ui::Take(shownRows * rowH);
		ui::List list = ui::BeginList(listArea, rowH, static_cast<int>(saved.size()), g.savedScroll);
		for (int i = list.first; i < list.last; i++) {
			const recents::Entry &e = saved[i];
			bool clicked;
			Rect row = ui::ListRow(list, i, g.selectedSaved == i, clicked);
			if (row.y < -1000) continue;
			ui::Spot("saved:" + std::to_string(i), row);
			int ty = row.y + (row.h - gfx::FontHeight(Font::Normal)) / 2;
			ui::TextIn(cx[0], ty, cx[1] - cx[0] - Px(10), Font::Bold, theme::kText, e.name);
			ui::TextIn(cx[1], ty, cx[2] - cx[1] - Px(10), Font::Normal, theme::kText, e.chamber + "  " + e.recordTime);
			gfx::Text(cx[2], ty, Font::Normal, theme::kText, Format("%.2f s", e.takeOver).c_str());
			std::string mode = e.mode == 1 ? "watch" : "practice";
			if (e.files.size() > 1 || e.role) mode += e.role ? "  \xC2\xB7  orange" : "  \xC2\xB7  blue";
			ui::TextIn(cx[3], ty, cx[4] - cx[3] - Px(10), Font::Small, theme::kDim, mode);
			ui::TextIn(cx[4], ty, row.x + row.w - cx[4], Font::Small, theme::kDim, recents::Ago(e.when));
			if (clicked) {
				bool twice = g.selectedSaved == i && GetTickCount() - g.lastClick < 400;
				g.selectedSaved = i;
				g.selectedRecent = -1;
				g.lastClick = GetTickCount();
				if (twice) {
					LoadEntry(e);
					ui::EndArea();
					return;
				}
			}
		}
		ui::EndList(list);
		ui::HintFor(listArea, "Double-click to load the run with its take-over, mode and speeds");
	}
	ui::Space(Px(10));

	// --- recent runs ---
	ui::Caption(Format("PRACTICED RECENTLY  (%d)", static_cast<int>(recent.size())));
	Rect head = ui::Take(Px(22));
	int w = head.w - Px(12);
	int cx[] = {head.x + Px(12), head.x + w * 30 / 100, head.x + w * 40 / 100, head.x + w * 52 / 100, head.x + w * 62 / 100, head.x + w * 76 / 100};
	const char *names[] = {"CHAMBER", "RECORD", "ATTEMPTS", "BEST", "WHEN", "FILE"};
	for (int c = 0; c < 6; c++) gfx::Text(cx[c], head.y + Px(2), Font::Small, theme::kFaint, names[c]);
	Rect listArea = ui::Area();
	listArea.h -= Px(52);
	ui::List list = ui::BeginList(listArea, rowH, static_cast<int>(recent.size()), g.recentScroll);
	for (int i = list.first; i < list.last; i++) {
		const recents::Entry &e = recent[i];
		bool clicked;
		Rect row = ui::ListRow(list, i, g.selectedRecent == i, clicked);
		if (row.y < -1000) continue;
		ui::Spot("recent:" + std::to_string(i), row);
		int ty = row.y + (row.h - gfx::FontHeight(Font::Normal)) / 2;
		ui::TextIn(cx[0], ty, cx[1] - cx[0] - Px(10), Font::Bold, theme::kText, e.chamber);
		gfx::Text(cx[1], ty, Font::Normal, theme::kText, e.recordTime.c_str());
		gfx::Text(cx[2], ty, Font::Normal, theme::kDim, std::to_string(e.attempts).c_str());
		gfx::Text(cx[3], ty, Font::Normal, !e.hasBest ? theme::kFaint : e.bestGap <= 0 ? theme::kGreen : theme::kOrange, gapText(e).c_str());
		ui::TextIn(cx[4], ty, cx[5] - cx[4] - Px(10), Font::Small, theme::kDim, recents::Ago(e.when));
		ui::TextIn(cx[5], ty, row.x + row.w - cx[5], Font::Small, theme::kDim, e.files.empty() ? "" : FileName(e.files[0]));
		if (clicked) {
			bool twice = g.selectedRecent == i && GetTickCount() - g.lastClick < 400;
			g.selectedRecent = i;
			g.selectedSaved = -1;
			g.lastClick = GetTickCount();
			if (twice) {
				LoadEntry(e);
				ui::EndArea();
				return;
			}
		}
	}
	ui::EndList(list);
	if (recent.empty()) ui::TextIn(listArea.x + Px(12), listArea.y + Px(8), listArea.w, Font::Small, theme::kDim, "Runs you load show up here.");
	ui::HintFor(listArea, "Double-click to load the run again; BEST is your best finish of that session against the record");

	// --- buttons ---
	Rect bottom{listArea.x, listArea.y + listArea.h + Px(12), listArea.w, Px(40)};
	bool haveSaved = g.selectedSaved >= 0 && g.selectedSaved < static_cast<int>(saved.size());
	bool haveRecent = g.selectedRecent >= 0 && g.selectedRecent < static_cast<int>(recent.size());
	if (ui::ButtonIn({bottom.x, bottom.y, Px(150), bottom.h}, "REMOVE", false, haveSaved || haveRecent)) {
		if (haveSaved) recents::RemoveSaved(g.selectedSaved);
		else recents::RemoveRecent(g.selectedRecent);
		g.selectedSaved = g.selectedRecent = -1;
	}
	ui::HintFor({bottom.x, bottom.y, Px(150), bottom.h}, "Take the selected run off its list (its demo files stay)");
	if (ui::ButtonIn({bottom.x + Px(160), bottom.y, Px(190), bottom.h}, "CLEAR RECENT", false, !recent.empty())) {
		recents::ClearRecent();
		g.selectedRecent = -1;
	}
	ui::HintFor({bottom.x + Px(160), bottom.y, Px(190), bottom.h}, "Empty the recently practiced list (saved runs stay)");
	if (ui::ButtonIn({bottom.x + bottom.w - Px(260), bottom.y, Px(260), bottom.h}, "LOAD THIS RUN", true, haveSaved || haveRecent))
		LoadEntry(haveSaved ? saved[g.selectedSaved] : recent[g.selectedRecent]);
	ui::EndArea();
}

// "<Portal 2>\chocoload.exe" for Steam's launch options (in front of a wrapper already there, such as P2Multi)
std::string SteamLaunchLine() {
	std::string game = engine::GameDirectory();
	size_t slash = game.find_last_of("\\/");
	std::string folder = slash == std::string::npos ? game : game.substr(0, slash);
	return "\"" + folder + "\\chocoload.exe\" %command%";
}

void CopyText(const std::string &text) {
	if (!OpenClipboard(nullptr)) return;
	EmptyClipboard();
	int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
	if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size * sizeof(wchar_t))) {
		MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, static_cast<wchar_t *>(GlobalLock(memory)), size);
		GlobalUnlock(memory);
		if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
	}
	CloseClipboard();
}

void SettingsPage(Rect area, const trainer::Status &s) {
	const trainer::Settings &set = trainer::CurrentSettings();
	int pad = Px(26);
	Rect left{area.x + pad, area.y + pad, area.w / 2 - pad - Px(14), area.h - 2 * pad};
	Rect right{area.x + area.w / 2 + Px(14), left.y, area.w / 2 - pad - Px(14), left.h};

	ui::BeginArea(left);
	ui::Caption("GHOSTS");
	bool ghost = set.ghost;
	if (ui::Toggle("Show the record's players as ghosts", ghost)) Set(Format("prac_set ghost %d", ghost ? 1 : 0));
	ui::Hint("The record's robots run beside you, so you see where you gain or lose");
	int style = set.ghostStyle == trainer::kGhostSolid ? 3 : set.ghostStyle == trainer::kGhostWire ? 2 : set.ghostStyle == trainer::kGhostWireLow ? 1 : 0;
	if (ui::Choice("STYLE", style, {"Basic", "Wire low", "Wire", "Solid"})) {
		static const char *const names[] = {"basic", "wirelow", "wire", "solid"};
		Set(Format("prac_set ghoststyle %s", names[style]));
	}
	ui::Hint("How the ghosts are drawn: Basic is cheapest, Solid shows the robots' models");
	int color = !set.ghostColorSet ? 0 : set.ghostColor[0] == 255 && set.ghostColor[1] == 255 && set.ghostColor[2] == 255 ? 1 : 2;
	if (ui::Choice("COLOUR", color, {"Robots' own", "White", "Green"})) {
		static const char *const values[] = {"team", "\"255 255 255\"", "\"80 230 120\""};
		Set(std::string("prac_set ghostcolor ") + values[color]);
	}
	ui::Hint("The ghosts' colour: the robots' own blue and orange, or one that stands out");
	float opacity = static_cast<float>(set.ghostOpacity);
	if (ui::Slider("OPACITY", opacity, 10, 100, "%.0f%%")) {
		// (shown at once; written to the settings file when the dragging has stopped, not on every frame of it)
		engine::Execute("prac_set ghostopacity %d", static_cast<int>(opacity + 0.5f));
		g.saveSettingsIn = kSaveAfterDrag;
	}
	ui::Hint("How see-through the ghosts are");
	ui::Caption("ON SCREEN");
	bool hudOn = set.hud, speedo = set.speedometer, path = set.path;
	if (ui::Toggle("Clock and gap to the record", hudOn)) Set(Format("prac_set hud %d", hudOn ? 1 : 0));
	ui::Hint("Your time and how far ahead of or behind the record you are, at the top of the screen");
	if (ui::Toggle("Speedometer", speedo)) Set(Format("prac_set speedometer %d", speedo ? 1 : 0));
	ui::Hint("Your speed next to the record's at the same point of the run");
	bool strafe = set.strafe;
	if (ui::Toggle("Bhop trainer: gain, strafe sync, hop timing", strafe)) Set(Format("prac_set strafe %d", strafe ? 1 : 0));
	ui::Hint("Any time you jump: speed gained, air time gaining speed, key against mouse sync, strafes, ticks lost on the ground");
	if (ui::Toggle("Route line and portal shots in the level", path)) Set(Format("prac_set path %d", path ? 1 : 0));
	ui::Hint("The record's path drawn in the level, with where its portals were shot");
	ui::Caption("MENU KEY");
	int key = g.menuKey;
	std::vector<std::string> keys = {"INSERT", "HOME", "F1", "F2", "F3", "F4"};
	int shownKey = -1;
	for (int i = 0; i < static_cast<int>(keys.size()); i++)
		if (keys[i] == kMenuKeys[key].name) shownKey = i;
	if (ui::Choice("", shownKey, keys)) {
		for (int i = 0; i < static_cast<int>(sizeof(kMenuKeys) / sizeof(kMenuKeys[0])); i++)
			if (keys[shownKey] == kMenuKeys[i].name) g.menuKey = i;
		SaveSettings();
	}
	ui::EndArea();

	ui::BeginArea(right);
	ui::Caption("RETRIES");
	int reset = set.reset == trainer::kResetSoft ? 1 : set.reset == trainer::kResetReload ? 2 : 0;
	if (ui::Choice("RESET", reset, {"Auto", "Instant", "Restart level"})) {
		static const char *const names[] = {"auto", "soft", "reload"};
		Set(Format("prac_set reset %s", names[reset]));
	}
	ui::Hint("How a retry goes back: Instant only moves the players back, Restart level reloads it, Auto picks what the level needs");
	float autoReset = set.autoReset;
	if (ui::Stepper("AUTO RETRY", autoReset, 0.5f, 0, 10, autoReset > 0 ? "%.1f s" : "off")) Set(Format("prac_set autoreset %g", autoReset));
	ui::Hint("Retry by itself once you are this many seconds behind the record (off: retry yourself with prac_retry)");
	bool challenge = set.challenge;
	if (ui::Toggle("Practice every run in Challenge Mode (flags)", challenge)) Set(Format("prac_set challenge %d", challenge ? 1 : 0));
	ui::Hint("Play every attempt in Challenge Mode, finishing at the flags like on the boards");

	ui::Caption("SAVE STATES");
	bool snapshots = set.snapshots;
	if (ui::Toggle("Retry through save states (ChocolateLoading)", snapshots)) Set(Format("prac_set snapshots %d", snapshots ? 1 : 0));
	ui::Hint("Retries put the whole game back to the take-over in about 10 ms instead of reloading");
	if (s.snapshotsReady) {
		ui::Label("ChocolateLoading is running: retries return in about 10 ms.", Font::Small, theme::kGreen);
	} else {
		ui::Label("ChocolateLoading must start with the game. Once, in Steam:", Font::Small, theme::kDim);
		ui::Label("Portal 2 > Properties > Launch Options, put this first:", Font::Small, theme::kDim);
		Rect line = ui::Take(Px(34));
		ui::Panel(line, theme::kField, theme::kLine);
		ui::TextIn(line.x + Px(10), line.y + (line.h - gfx::FontHeight(Font::Small)) / 2, line.w - Px(20), Font::Small, theme::kText, SteamLaunchLine());
		if (ui::Button("COPY THE LAUNCH OPTION")) {
			CopyText(SteamLaunchLine());
			Note("Copied: paste it in front of Portal 2's launch options in Steam, then restart the game.", 10);
		}
		std::error_code ec;
		std::string folder = engine::GameDirectory();
		folder = folder.substr(0, folder.find_last_of("\\/"));
		if (!std::filesystem::exists(std::filesystem::u8path(folder + "\\chocoload.exe"), ec))
			ui::Label("chocoload.exe isn't in the Portal 2 folder yet: copy it there from P2 Practice's chocolateloading folder.", Font::Small, theme::kOrange);
		ui::Label("With another wrapper (P2Multi): \"...P2Multi.exe\" \"...chocoload.exe\" %command% ...", Font::Small, theme::kFaint);
	}

	// sets of settings kept under a name, to switch between (say, one for learning and one for runs)
	ui::Caption("SAVED SETTINGS");
	static std::vector<std::string> profiles;
	static DWORD profilesRead = 0;
	if (profilesRead == 0 || GetTickCount() - profilesRead > 2000) {
		profiles = Profiles();
		profilesRead = GetTickCount() | 1;
	}
	Rect row = ui::Take(Px(34));
	ui::BeginArea({row.x, row.y, row.w - Px(130), row.h});
	bool entered = ui::TextBox("profilename", g.profileName, "Name for these settings");
	ui::Hint("Every setting on this page (and the menu key) is saved under this name");
	ui::EndArea();
	Rect saveButton{row.x + row.w - Px(120), row.y, Px(120), row.h};
	if (ui::ButtonIn(saveButton, "SAVE") || entered) {
		std::string file = ProfileFileName(g.profileName);
		if (file.empty()) g.profileNote = "Type a name first (letters, digits, - and _).";
		else {
			SaveSettings(ProfilesFolder() + "\\" + file + ".cfg");
			g.profileNote = "Saved as " + file + ".";
			g.profileName.clear();
			ui::Unfocus();
			profilesRead = 0;
		}
	}
	if (!g.profileNote.empty()) ui::Label(g.profileNote, Font::Small, theme::kDim);
	for (const std::string &name : profiles) {
		Rect r = ui::Take(Px(30));
		if (r.y + r.h > right.y + right.h) break;
		ui::TextIn(r.x + Px(6), r.y + (r.h - gfx::FontHeight(Font::Normal)) / 2, r.w - Px(250), Font::Normal, theme::kText, name);
		Rect load{r.x + r.w - Px(240), r.y, Px(115), r.h}, remove{r.x + r.w - Px(115), r.y, Px(115), r.h};
		if (ui::ButtonIn(load, "LOAD")) {
			engine::Execute("exec p2practice/profiles/%s", name.c_str());
			g.saveSettingsIn = 3;  // (and they are the settings from now on, once the game has run the file)
			g.profileNote = "Loaded " + name + ".";
		}
		ui::HintFor(load, "Switch to the settings saved as " + name);
		if (ui::ButtonIn(remove, "DELETE")) {
			std::error_code ec;
			std::filesystem::remove(std::filesystem::u8path(ProfilesFolder() + "\\" + name + ".cfg"), ec);
			g.profileNote = "Deleted " + name + ".";
			profilesRead = 0;
			break;
		}
	}
	ui::EndArea();
}

// ---- the scripted tour (prac_menu_tour): the menu used by a stand-in cursor, for showing it in a video ----
struct Step {
	char kind;      // c command, m move to a spot, k click, d double click, w wait, t type, h wheel, u until, e end
	std::string a;  // the command, spot, text or condition
	int n = 0;      // frames (move, wait, per typed character), wheel steps, or the most frames to wait (until)
};

struct Tour {
	bool on = false;
	std::vector<Step> steps;
	size_t step = 0;
	int frame = 0;
	float x = 0, y = 0, fromX = 0, fromY = 0;
	bool left = false;
	int waited = 0;
	int frames = 0;  // frames drawn since the tour began (the video's frame number)
	std::string record;  // the video SAR renders the tour into ("" for none)
} g_tour;

const std::vector<Step> &TourScript() {
	static const std::vector<Step> script = {
		{'c', "prac_menu practice"}, {'w', "", 50},
		{'m', "RUNS", 30}, {'k'}, {'w', "", 30},
		{'m', "search", 25}, {'k'}, {'t', "flings", 6}, {'w', "", 55},
		{'m', "BOARDS", 30}, {'k'}, {'w', "", 25},
		{'m', "chambers", 20}, {'h', "", -20}, {'w', "", 20},
		{'m', "chamber:Flings", 25}, {'k'}, {'w', "", 10},
		{'m', "entry:0", 35}, {'w', "", 25}, {'d'},
		{'u', "page:practice", 900}, {'w', "", 50},
		{'m', "Watch the run", 28}, {'k'}, {'w', "", 15},
		{'m', "part:0", 25}, {'k'}, {'w', "", 25},
		{'m', "start", 28}, {'k'},
		{'u', "mode:lead-in|watch|run", 3000}, {'w', "", 330},
		{'c', "prac_menu"}, {'w', "", 20},
		{'m', "SETTINGS", 25}, {'k'}, {'w', "", 20},
		{'m', "Solid", 22}, {'k'}, {'w', "", 35},
		{'c', "prac_menu close"},
		{'u', "mode:done", 1200}, {'w', "", 60},
		{'e'},
	};
	return script;
}

// Development (prac_menu_tour takeover): types a take-over time into the PRACTICE page's slider, with a screenshot
// while typing and one after a click elsewhere set it. A run must be loaded.
const std::vector<Step> &TakeOverScript() {
	static const std::vector<Step> script = {
		{'c', "prac_menu practice"}, {'w', "", 20},
		{'m', "value:TAKE OVER AT", 20}, {'k'},
		{'t', "8.5", 4}, {'w', "", 10}, {'c', "jpeg takeover_typing 90"}, {'w', "", 10},
		{'m', "Practice", 20}, {'k'}, {'w', "", 10}, {'c', "jpeg takeover_set 90"}, {'w', "", 10},
		{'c', "prac_menu close"}, {'e'},
	};
	return script;
}

// Development (prac_menu_tour boards): picks a chamber and another at once, while the first board is still
// coming; a screenshot once the second is up (its own runs under its name, not the first's).
const std::vector<Step> &BoardsRaceScript() {
	static const std::vector<Step> script = {
		{'c', "prac_menu boards"}, {'w', "", 20},
		{'m', "chamber:Container Ride", 15}, {'k'},
		{'m', "chamber:Portal Carousel", 8}, {'k'},
		{'w', "", 400}, {'c', "jpeg boards_race 90"}, {'w', "", 10},
		{'c', "prac_menu close"}, {'e'},
	};
	return script;
}

// Development (prac_menu_tour ui): every page, with a screenshot of each and of a hint, a sort, a filter and the
// chamber search
const std::vector<Step> &UiScript() {
	static const std::vector<Step> script = {
		{'c', "prac_menu practice"}, {'w', "", 20},
		{'m', "Watch the run", 20}, {'w', "", 10}, {'c', "jpeg ui_practice 90"}, {'w', "", 10},
		{'m', "start", 20}, {'w', "", 5}, {'m', "part:0", 10}, {'w', "", 5},
		{'m', "savename", 15}, {'k'}, {'t', "laser warmup", 2}, {'m', "SAVE", 15}, {'k'}, {'w', "", 10},
		{'c', "prac_menu recent"}, {'w', "", 20}, {'m', "recent:0", 15}, {'k'}, {'w', "", 10}, {'c', "jpeg ui_recent 90"}, {'w', "", 10},
		{'c', "prac_menu runs"}, {'w', "", 30},
		{'m', "sort:TIME", 20}, {'k'}, {'w', "", 10},
		{'m', "THIS MAP", 20}, {'k'}, {'w', "", 15}, {'c', "jpeg ui_runs 90"}, {'w', "", 10},
		{'k'}, {'m', "sort:CHAMBER", 15}, {'k'}, {'w', "", 5},
		{'c', "prac_menu boards"}, {'w', "", 240}, {'c', "jpeg ui_boards 90"}, {'w', "", 10},
		{'m', "chamberfilter", 20}, {'k'}, {'t', "fling", 4}, {'w', "", 15}, {'c', "jpeg ui_boards_filter 90"}, {'w', "", 10},
		{'c', "prac_menu settings"}, {'w', "", 20},
		{'m', "Retry through save states (ChocolateLoading)", 20}, {'w', "", 10}, {'c', "jpeg ui_settings 90"}, {'w', "", 10},
		{'c', "prac_menu close"}, {'e'},
	};
	return script;
}

// Development (prac_menu_tour saved): loads the first saved run from the RECENT page (screenshot of the practice
// page it sets), then saves the settings as "runs", picks another ghost style and loads "runs" back (screenshots)
const std::vector<Step> &SavedScript() {
	static const std::vector<Step> script = {
		{'c', "prac_menu recent"}, {'w', "", 20}, {'m', "saved:0", 15}, {'d'}, {'w', "", 30}, {'c', "jpeg saved_loaded 90"}, {'w', "", 10},
		{'c', "prac_menu settings"}, {'w', "", 20}, {'m', "profilename", 15}, {'k'}, {'t', "runs", 2}, {'m', "SAVE", 15}, {'k'}, {'w', "", 15},
		{'m', "Wire", 15}, {'k'}, {'w', "", 10}, {'c', "jpeg profile_changed 90"}, {'w', "", 10},
		{'m', "LOAD", 15}, {'k'}, {'w', "", 20}, {'c', "jpeg profile_loaded 90"}, {'w', "", 10},
		{'c', "prac_menu close"}, {'e'},
	};
	return script;
}

void EndTour() {
	if (!g_tour.on) return;
	g_tour.on = false;
	gfx::SetPuppet(false);
	if (!g_tour.record.empty()) engine::Execute("sar_render_finish; host_framerate 0");
	engine::Print("Menu tour finished.\n");
}

void CmdTour(const sdk::CCommand &args) {
	if (args.ArgC() >= 2 && !_stricmp(args.Arg(1), "stop")) return EndTour();
	g_tour = Tour{};
	g_tour.on = true;
	bool takeOver = args.ArgC() >= 2 && !_stricmp(args.Arg(1), "takeover");
	bool boardsRace = args.ArgC() >= 2 && !_stricmp(args.Arg(1), "boards");
	bool uiPages = args.ArgC() >= 2 && !_stricmp(args.Arg(1), "ui");
	bool savedRuns = args.ArgC() >= 2 && !_stricmp(args.Arg(1), "saved");
	g_tour.steps = takeOver ? TakeOverScript() : boardsRace ? BoardsRaceScript() : uiPages ? UiScript() : savedRuns ? SavedScript() : TourScript();
	g_tour.x = g_tour.fromX = gfx::ScreenWidth() * 0.5f;
	g_tour.y = g_tour.fromY = gfx::ScreenHeight() * 0.6f;
	gfx::SetPuppet(true);
	// prac_menu_tour record <video>: rendered by SAR at 60 frames a second, one frame per frame of the tour
	if (args.ArgC() >= 3 && !_stricmp(args.Arg(1), "record")) {
		g_tour.record = args.Arg(2);
		engine::Execute("sv_cheats 1; host_framerate 60");
		engine::Execute("sar_render_start \"%s\"", g_tour.record.c_str());
	}
}

float Ease(float t) { return t < 0.5f ? 4 * t * t * t : 1 - (-2 * t + 2) * (-2 * t + 2) * (-2 * t + 2) / 2; }

// One frame of the tour: moves the stand-in cursor and presses its button
void TickTour() {
	if (!g_tour.on) return;
	if (g_tour.step >= g_tour.steps.size()) return EndTour();
	const Step &step = g_tour.steps[g_tour.step];
	bool next = false;
	int f = g_tour.frame++;
	if (f == 0 && g_tour.waited == 0) engine::Print("[tour] frame %d: step %d %c %s\n", g_tour.frames, static_cast<int>(g_tour.step), step.kind, step.a.c_str());
	g_tour.frames++;
	switch (step.kind) {
	case 'c':
		engine::Execute("%s", step.a.c_str());
		next = true;
		break;
	case 'w': next = f + 1 >= step.n; break;
	case 'm': {
		Rect r;
		if (!ui::FindSpot(step.a, r)) {
			// (not drawn yet: a board still loading; the move starts once it is there)
			g_tour.frame = 0;
			if (++g_tour.waited > 900) next = true;
			break;
		}
		if (f == 0) {
			g_tour.fromX = g_tour.x;
			g_tour.fromY = g_tour.y;
		}
		float tx = r.x + std::min(r.w / 2.0f, static_cast<float>(Px(60))), ty = r.y + r.h / 2.0f;
		float t = Ease(std::min(1.0f, (f + 1) / static_cast<float>(std::max(1, step.n))));
		g_tour.x = g_tour.fromX + (tx - g_tour.fromX) * t;
		g_tour.y = g_tour.fromY + (ty - g_tour.fromY) * t;
		next = f + 1 >= step.n;
		break;
	}
	case 'k':
		g_tour.left = f < 3;
		next = f >= 5;
		break;
	case 'd':
		g_tour.left = f < 2 || (f >= 5 && f < 7);
		next = f >= 9;
		break;
	case 't':
		if (f % std::max(1, step.n) == 0) {
			size_t i = f / std::max(1, step.n);
			if (i < step.a.size()) gfx::PuppetInput(static_cast<int>(g_tour.x), static_cast<int>(g_tour.y), g_tour.left, 0, step.a.substr(i, 1));
			else next = true;
		}
		break;
	case 'h':
		if (f % 3 == 0) {
			int done = f / 3, total = std::abs(step.n);
			if (done < total) gfx::PuppetInput(static_cast<int>(g_tour.x), static_cast<int>(g_tour.y), g_tour.left, step.n < 0 ? -1 : 1, "");
			else next = true;
		}
		break;
	case 'u': {
		bool met = false;
		if (step.a.compare(0, 5, "page:") == 0) met = g.page == Page::Practice && step.a == "page:practice";
		else if (step.a.compare(0, 5, "mode:") == 0) {
			// "mode:watch|lead-in": any of them
			std::string mode = trainer::CurrentStatus().mode, wanted = "|" + step.a.substr(5) + "|";
			met = wanted.find("|" + mode + "|") != std::string::npos;
		}
		next = met || f >= step.n;
		break;
	}
	case 'e': return EndTour();
	}
	gfx::PuppetInput(static_cast<int>(g_tour.x), static_cast<int>(g_tour.y), g_tour.left, 0, "");
	if (next) {
		g_tour.step++;
		g_tour.frame = 0;
		g_tour.waited = 0;
		g_tour.left = false;
	}
}

// An arrow cursor at the mouse (the game draws none of its own in a level)
void DrawCursor() {
	int x = gfx::MouseX(), y = gfx::MouseY(), size = Px(22);
	for (int row = 0; row < size; row++) {
		int width = row * 11 / 16 + 1;
		if (row > size * 3 / 4) width = std::max(1, (size - row) * 2);
		gfx::FillRect(x - 1, y + row, width + 2, 1, Color(0, 0, 0, 200));
		gfx::FillRect(x, y + row, width, 1, Color(255, 255, 255));
	}
}

void Draw() {
	TickTour();
	hud::Draw();
	if (g_tour.on && !g.open) DrawCursor();
	if (g.saveSettingsIn > 0 && --g.saveSettingsIn == 0) SaveSettings();
	FinishDownload();
	// (the recent list, a few times a second: enough to catch every load and finish)
	static int recentFrames = 0;
	if (++recentFrames >= 15) {
		recentFrames = 0;
		TrackRecents(trainer::CurrentStatus());
	}
	if (gfx::ConsoleVisible()) return;
	// the menu's key, and Esc to close
	if (!ui::AnyTextFocused() && gfx::KeyPressed(kMenuKeys[g.menuKey].code)) Open(!g.open);
	else if (g.open && !ui::AnyTextFocused() && gfx::KeyPressed(gfx::kKeyEscape)) Open(false);
	if (!g.open) return;
	// Tab: the next page (Shift: the one before)
	if (!ui::AnyTextFocused() && gfx::KeyPressed(gfx::kKeyTab)) {
		bool back = gfx::KeyDown(gfx::kKeyLShift) || gfx::KeyDown(gfx::kKeyRShift);
		g.page = static_cast<Page>((static_cast<int>(g.page) + (back ? kPages - 1 : 1)) % kPages);
	}

	ui::BeginFrame();
	int sw = gfx::ScreenWidth(), sh = gfx::ScreenHeight();
	gfx::FillRect(0, 0, sw, sh, theme::kBackdrop);
	int w = std::min(Px(1240), sw - Px(40)), h = std::min(Px(800), sh - Px(40));
	Rect window{(sw - w) / 2, (sh - h) / 2, w, h};
	ui::Panel(window, theme::kWindow, theme::kLine);
	trainer::Status s = trainer::CurrentStatus();
	Header(window);
	Rect body{window.x, window.y + Px(58), window.w, window.h - Px(58) - Px(34)};
	switch (g.page) {
	case Page::Practice: PracticePage(body, s); break;
	case Page::Recent: RecentPage(body); break;
	case Page::Runs: RunsPage(body); break;
	case Page::Boards: BoardsPage(body); break;
	case Page::Settings: SettingsPage(body, s); break;
	}
	Footer(window, s);
	// a cursor of the menu's own where the game draws none (in a level), and for the tour
	if (gfx::InGame() || g_tour.on) DrawCursor();
}

}  // namespace

bool Init() {
	if (!gfx::Init()) return false;
	engine::RegisterCommand("prac_menu", CmdMenu, "prac_menu - open or close P2 Practice's menu");
	engine::RegisterCommand("prac_menukey", CmdMenuKey, "prac_menukey <key> - the key that opens P2 Practice's menu (INSERT, HOME, RSHIFT, F1-F12)");
	engine::RegisterCommand("prac_folder_add", CmdFolderAdd, "prac_folder_add <folder> - a folder of demos for the menu's RUNS page");
	engine::RegisterCommand("prac_menu_tour", CmdTour, "prac_menu_tour [record <video> | stop] - a scripted walk through the menu, for showing it");
	engine::RegisterCommand("prac_folders", CmdFolders, "prac_folders - the folders the menu's RUNS page reads");
	gfx::SetDrawCallback(Draw);
	trainer::UseDrawnHud(true);
	std::error_code ec;
	if (std::filesystem::exists(std::filesystem::u8path(SettingsPath()), ec)) engine::Execute("exec p2practice/settings");
	g.ready = true;
	return true;
}

void Shutdown() {
	if (!g.ready) return;
	EndTour();
	if (g.saveSettingsIn > 0) SaveSettings();  // (a change still waiting to be written)
	trainer::UseDrawnHud(false);
	gfx::Shutdown();
	library::Shutdown();
	boards::Shutdown();
	g = State{};
}

}  // namespace menu
