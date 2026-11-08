// Chamber names for showing a map to people: "Doors" rather than mp_coop_doors.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace maps {

// The chamber's name as the leaderboards have it; the file name itself for a map that isn't one of the game's.
std::string ChamberName(const std::string &mapFile);
// The chamber's place in the game (single player first, then co-op); after all of them for other maps
int ChamberOrder(const std::string &mapFile);
// The game's chambers in order: (map file, name), single player or co-op
std::vector<std::pair<std::string, std::string>> Chambers(bool coop);

}  // namespace maps
