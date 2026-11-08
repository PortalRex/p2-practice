// The runs on this computer: every demo in the player's folders, read on a background thread (map, recording
// player, time) and kept in a cache file, with the two demos of a co-op run put together as one run. The menu's
// RUNS page shows them; loading one is prac_load with its demo(s).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace library {

struct Demo {
	std::string path, name;  // name: the file name without ".dem"
	int64_t size = 0, modified = 0;
	bool ok = false;
	std::string map, player, error;
	bool coop = false, host = false, hasTime = false;
	int ticks = 0;
	float seconds = 0;
};

struct Run {
	const Demo *demo = nullptr;     // the only demo, or the host's of a co-op pair
	const Demo *partner = nullptr;  // the client's demo of the same run
	std::string chamber, players, mode, files, timeText, search;  // shown (search: lower case, everything)
	int order = 0;                  // the chamber's place in the game
};

// The data folder (<game>/p2practice) and the folders searched for demos
std::string DataFolder();
std::string DownloadFolder();   // where the boards' demos go: <game>/demos/p2practice/boards
const std::vector<std::string> &Folders();
bool AddFolder(const std::string &path);  // false when it doesn't exist or is listed already
void RemoveFolder(size_t index);

// Reads the folders again (in the background). Started by the first call to Runs too.
void Refresh();
void Shutdown();

// The runs as read so far; `version` changes whenever they do
const std::vector<Run> &Runs();
int Version();
bool Busy();
void Progress(int &done, int &total);

// prac_load's arguments for a run: its demo(s), quoted
std::string LoadCommand(const Run &run);

}  // namespace library
