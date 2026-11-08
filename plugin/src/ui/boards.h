// The Challenge Mode leaderboards at board.portal2.sr, as a source of demos: a chamber's board (every run on it,
// with who, when and whether a demo is there) and the demos themselves, downloaded into the library's download
// folder. Co-op runs are two entries on the board, one per robot, each with its own demo; they are shown as one.
// Everything that goes over the network runs on a background thread; the menu polls.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace boards {

struct Entry {
	int rank = 0;
	int score = 0;  // hundredths of a second
	std::string player, steamId, date;  // date: "2024-05-01"
	int changelogId = 0;
	bool hasDemo = false;
	std::string partnerPlayer, partnerSteamId;  // co-op: the other robot's entry
	int partnerChangelogId = 0;
	bool partnerHasDemo = false;
	bool coop = false;

	bool Complete() const { return hasDemo && (!coop || partnerChangelogId == 0 || partnerHasDemo); }
	std::string Players() const { return partnerPlayer.empty() ? player : player + "  +  " + partnerPlayer; }
};

struct Chapter {
	std::string name;
	std::vector<std::pair<std::string, std::string>> chambers;  // map file, shown name
};
const std::vector<Chapter> &Chapters();
bool OnBoard(const std::string &map);
std::string FormatScore(int hundredths);

enum class State { Idle, Busy, Ready, Failed };

// A chamber's board, fetched in the background
void Fetch(const std::string &map);
State BoardState();
const std::string &BoardMap();
const std::vector<Entry> &Entries();  // best first (valid when Ready)
const std::string &BoardError();

// A run's demo(s), downloaded in the background; the files when Ready
bool Downloaded(const std::string &map, const Entry &entry);
void Download(const std::string &map, const Entry &entry);
State DownloadState();
const std::vector<std::string> &DownloadedFiles();
const std::string &DownloadError();

void Shutdown();

}  // namespace boards
