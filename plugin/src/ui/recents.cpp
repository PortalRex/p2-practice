#include "ui/recents.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "ui/library.h"

namespace recents {
namespace {

constexpr size_t kRecentKept = 40;
constexpr const char *kHeader = "p2practice recents 1";

std::vector<Entry> g_recent, g_saved;
bool g_loaded = false;

std::string Path() { return library::DataFolder() + "\\recents.txt"; }

std::vector<std::string> Split(const std::string &line, char separator) {
	std::vector<std::string> parts;
	std::string part;
	std::istringstream in(line);
	while (std::getline(in, part, separator)) parts.push_back(part);
	return parts;
}

std::string Join(const std::vector<std::string> &parts, char separator) {
	std::string out;
	for (size_t i = 0; i < parts.size(); i++) out += (i ? std::string(1, separator) : "") + parts[i];
	return out;
}

// One line: kind, when, name, chamber, map, record, attempts, best (or -), take-over, mode, role, partner,
// lead-in, speed, files (|-separated). Tabs and line ends never appear in the values (they come from file names
// and the menu's own text boxes, which take neither).
std::string Line(const Entry &e) {
	char numbers[160];
	snprintf(numbers, sizeof(numbers), "%lld\t", static_cast<long long>(e.when));
	std::string best = e.hasBest ? std::to_string(e.bestGap) : "-";
	char choices[160];
	snprintf(choices, sizeof(choices), "%g\t%d\t%d\t%d\t%g\t%g", e.takeOver, e.mode, e.role, e.partner, e.leadIn, e.speed);
	return std::string(e.saved ? "S" : "R") + "\t" + numbers + e.name + "\t" + e.chamber + "\t" + e.map + "\t" + e.recordTime + "\t" +
	       std::to_string(e.attempts) + "\t" + best + "\t" + choices + "\t" + Join(e.files, '|');
}

bool Parse(const std::string &line, Entry &e) {
	std::vector<std::string> f = Split(line, '\t');
	if (f.size() < 15 || (f[0] != "S" && f[0] != "R")) return false;
	e.saved = f[0] == "S";
	e.when = std::atoll(f[1].c_str());
	e.name = f[2];
	e.chamber = f[3];
	e.map = f[4];
	e.recordTime = f[5];
	e.attempts = std::atoi(f[6].c_str());
	e.hasBest = f[7] != "-";
	e.bestGap = e.hasBest ? static_cast<float>(std::atof(f[7].c_str())) : 0;
	e.takeOver = static_cast<float>(std::atof(f[8].c_str()));
	e.mode = std::atoi(f[9].c_str());
	e.role = std::atoi(f[10].c_str());
	e.partner = std::atoi(f[11].c_str());
	e.leadIn = static_cast<float>(std::atof(f[12].c_str()));
	e.speed = static_cast<float>(std::atof(f[13].c_str()));
	e.files = Split(f[14], '|');
	return !e.files.empty();
}

void Load() {
	if (g_loaded) return;
	g_loaded = true;
	std::ifstream in(std::filesystem::u8path(Path()));
	std::string line;
	if (!std::getline(in, line) || line != kHeader) return;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		Entry e;
		if (Parse(line, e)) (e.saved ? g_saved : g_recent).push_back(std::move(e));
	}
}

void Store() {
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::u8path(library::DataFolder()), ec);
	// (written next to the old file and swapped in: a game closing halfway leaves the old list, not half of one)
	std::string temporary = Path() + ".new";
	{
		std::ofstream out(std::filesystem::u8path(temporary), std::ios::trunc);
		out << kHeader << "\n";
		for (const Entry &e : g_saved) out << Line(e) << "\n";
		for (const Entry &e : g_recent) out << Line(e) << "\n";
		if (!out) return;
	}
	std::filesystem::rename(std::filesystem::u8path(temporary), std::filesystem::u8path(Path()), ec);
}

int64_t Now() { return static_cast<int64_t>(std::time(nullptr)); }

}  // namespace

const std::vector<Entry> &Recent() {
	Load();
	return g_recent;
}

const std::vector<Entry> &Saved() {
	Load();
	return g_saved;
}

void Touch(const Entry &run) {
	Load();
	Entry e = run;
	e.saved = false;
	e.name.clear();
	e.when = Now();
	e.attempts = 0;
	e.hasBest = false;
	g_recent.erase(std::remove_if(g_recent.begin(), g_recent.end(), [&](const Entry &o) { return o.files == e.files; }), g_recent.end());
	g_recent.insert(g_recent.begin(), std::move(e));
	if (g_recent.size() > kRecentKept) g_recent.resize(kRecentKept);
	Store();
}

void UpdateSession(const std::vector<std::string> &files, int attempts, bool hasBest, float bestGap) {
	Load();
	if (g_recent.empty() || g_recent.front().files != files) return;
	Entry &e = g_recent.front();
	if (e.attempts == attempts && e.hasBest == hasBest && e.bestGap == bestGap) return;
	e.attempts = attempts;
	e.hasBest = hasBest;
	e.bestGap = bestGap;
	e.when = Now();
	Store();
}

void Save(const Entry &run) {
	Load();
	Entry e = run;
	e.saved = true;
	e.when = Now();
	g_saved.erase(std::remove_if(g_saved.begin(), g_saved.end(), [&](const Entry &o) { return o.name == e.name; }), g_saved.end());
	g_saved.insert(g_saved.begin(), std::move(e));
	Store();
}

void RemoveSaved(size_t index) {
	Load();
	if (index >= g_saved.size()) return;
	g_saved.erase(g_saved.begin() + index);
	Store();
}

void RemoveRecent(size_t index) {
	Load();
	if (index >= g_recent.size()) return;
	g_recent.erase(g_recent.begin() + index);
	Store();
}

void ClearRecent() {
	Load();
	g_recent.clear();
	Store();
}

std::string Ago(int64_t when) {
	int64_t seconds = std::max<int64_t>(0, Now() - when);
	char text[48];
	if (seconds < 60) return "just now";
	if (seconds < 3600) snprintf(text, sizeof(text), "%lld min ago", static_cast<long long>(seconds / 60));
	else if (seconds < 86400) snprintf(text, sizeof(text), "%lld h ago", static_cast<long long>(seconds / 3600));
	else if (seconds < 2 * 86400) return "yesterday";
	else if (seconds < 7 * 86400) snprintf(text, sizeof(text), "%lld days ago", static_cast<long long>(seconds / 86400));
	else {
		time_t t = static_cast<time_t>(when);
		tm local{};
		localtime_s(&local, &t);
		strftime(text, sizeof(text), "%d %b %Y", &local);
	}
	return text;
}

}  // namespace recents
