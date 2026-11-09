// What the player practiced lately and the runs they saved, for the menu's RECENT page. Kept in
// <game>/p2practice/recents.txt, one run per line, so it outlives the game.
//
// A recent run is added (or moved to the top) whenever a run is loaded, and carries the session's attempts and
// best finish. A saved run is one the player kept on purpose, together with how they practice it (where they
// take over, watch or practice, the robot and partner, lead-in and game speed); saved runs stay until removed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace recents {

struct Entry {
	bool saved = false;
	std::string name;        // the saved run's name ("" for a recent one: the chamber is shown)
	std::string chamber, map, recordTime;
	std::vector<std::string> files;  // its demo(s)
	int64_t when = 0;        // last practiced or saved (seconds since 1970)
	int attempts = 0;        // in the last session with it
	float bestGap = 0;       // the best finish of that session against the record
	bool hasBest = false;
	// how it was practiced (saved runs)
	float takeOver = 0;
	int mode = 0, role = 0, partner = 0;
	float leadIn = 1.5f, speed = 1;
};

const std::vector<Entry> &Recent();  // newest first
const std::vector<Entry> &Saved();   // newest first

// A run was loaded (files: its demos): to the top of the recent list
void Touch(const Entry &run);
// The session of the run on top has moved on: its attempts and best
void UpdateSession(const std::vector<std::string> &files, int attempts, bool hasBest, float bestGap);
void Save(const Entry &run);  // replaces a saved run of the same name
void RemoveSaved(size_t index);
void RemoveRecent(size_t index);
void ClearRecent();

// "3 min ago", "yesterday", "12 Oct"
std::string Ago(int64_t when);

}  // namespace recents
