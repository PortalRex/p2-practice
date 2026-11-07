// Wireframe copies of the player models, for ghosts drawn as lines.
//
// The game draws a model with the materials its file names, and offers no way to swap them on one entity. So
// the plugin makes its own copies of the three player models from the game's own files (the pak01 archive),
// pointing at wireframe materials it writes beside them, under the game folder: models/p2practice/ and
// materials/p2practice/ghost/. The copies animate like the originals (they share the animation files). A
// "coarse" copy draws the model's coarsest level of detail whatever the distance, far fewer lines; a "basic"
// one drops that mesh's small triangles too. Chell's model has only the one level and is left whole.
#pragma once

#include <string>

namespace ghostmodel {

// The model path to give a ghost of `team`'s robot (route::kTeamBlue, kTeamOrange, else Chell): the wireframe
// copy, made first if it doesn't exist yet. "" when it can't be made (the reason is printed once per session).
enum Detail { kFull = 0, kCoarse = 1, kBasic = 2 };
std::string Wireframe(int team, Detail detail);

}  // namespace ghostmodel
