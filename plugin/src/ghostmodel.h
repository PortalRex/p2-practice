// Wireframe copies of the player models, for ghosts drawn as lines.
//
// The game draws a model with the materials its file names, and offers no way to swap them on one entity. So
// the plugin makes its own copies of the three player models from the game's own files (the pak01 archive),
// pointing at wireframe materials it writes beside them, under the game folder: models/p2practice/ and
// materials/p2practice/ghost/. The copies animate like the originals (they share the animation files). A
// "low detail" copy's mesh file is told to draw the model's coarsest level of detail at any distance, which
// draws far fewer lines (Chell's model has only the one level).
#pragma once

#include <string>

namespace ghostmodel {

// The model path to give a ghost of `team`'s robot (route::kTeamBlue, kTeamOrange, else Chell): the wireframe
// copy, made first if it doesn't exist yet. "" when it can't be made (the reason is printed once per session).
std::string Wireframe(int team, bool lowDetail);

}  // namespace ghostmodel
