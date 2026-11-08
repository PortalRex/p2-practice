// The plugin's in-game menu (the launcher's job, inside the game): pick a run from the demos on this computer or
// from the boards, choose how to practice it, change the settings, set up save states. Opened and closed with its
// key (F1 unless changed) or prac_menu; it also draws the practice HUD.
#pragma once

namespace menu {

// Installs the drawing (gfx) and the menu's commands. False when the game's drawing couldn't be hooked: the
// plugin then works from the console and with the game's text HUD, as before.
bool Init();
void Shutdown();

}  // namespace menu
