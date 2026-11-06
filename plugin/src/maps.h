// Chamber names for showing a map to people: "Doors" rather than mp_coop_doors.
#pragma once

#include <string>

namespace maps {

// The chamber's name as the leaderboards have it; the file name itself for a map that isn't one of the game's.
std::string ChamberName(const std::string &mapFile);

}  // namespace maps
