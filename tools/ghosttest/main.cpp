// Development check for the wireframe ghost models: makes the copies for a game folder, as the plugin would,
// without the game. The copies are written under that folder, so point it at a test profile (or a scratch
// folder beside a Portal 2 install), not at the real portal2 folder.
// Usage: ghosttest <game folder, e.g. ...\Portal 2\p2practice-test\t1\portal2> [low]
#include <cstdarg>
#include <cstdio>
#include <string>

#include "ghostmodel.h"

namespace engine {
static std::string g_dir;
std::string GameDirectory() { return g_dir; }
void Warn(const char *format, ...) {
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}
}  // namespace engine

int main(int argc, char **argv) {
	if (argc < 2) return 1;
	engine::g_dir = argv[1];
	bool low = argc > 2;
	for (int team : {3, 2, 0}) printf("team %d: %s\n", team, ghostmodel::Wireframe(team, low).c_str());
	return 0;
}
