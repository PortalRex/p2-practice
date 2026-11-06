// The practice session: loads a route, puts the players on it, runs the ghost, the partner bot, the clock and the
// on-screen text, and resets between attempts.
#pragma once

#include "sdk.h"

namespace trainer {

void Init();      // registers the prac_ console commands
void Shutdown();  // ends any session and gives the game back as it was

void OnLevelInit(const char *mapName);
void OnLevelShutdown();
void OnFrame(bool simulating);  // once per server tick

// A prac_ command typed by a player who doesn't have the plugin (the co-op partner): true when handled.
bool OnClientCommand(const sdk::CCommand &args);

}  // namespace trainer
