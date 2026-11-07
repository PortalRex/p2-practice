// p2route: describes demos as JSON for the launcher (and for people, with --text).
//
//   p2route scan <demo>                  quick: map, recording player, run time
//   p2route info <demo> [partner demo]   full: players, parts, notes
//   p2route info --text <demo> [...]     the same as readable text
//
// Prints one JSON object on stdout; exit code 0 on success, 1 on failure ({"ok":false,"error":"..."}).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "p2demo/demo.h"
#include "route.h"

namespace {

std::string Json(const std::string &text) {
	std::string out = "\"";
	for (unsigned char c : text) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char escape[8];
				snprintf(escape, sizeof(escape), "\\u%04x", c);
				out += escape;
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	return out + "\"";
}

int Fail(const std::string &error) {
	printf("{\"ok\":false,\"error\":%s}\n", Json(error).c_str());
	return 1;
}

const char *TeamKey(int team) {
	return team == route::kTeamBlue ? "blue" : team == route::kTeamOrange ? "orange" : "";
}

int Scan(const char *path) {
	p2demo::Demo demo;
	std::string error;
	p2demo::ParseOptions options;
	options.entities = false;  // the header and SAR's data are enough here, and far quicker to read
	if (!p2demo::ParseFile(path, demo, error, options)) return Fail(error);

	int ticks = 0;
	for (const auto &result : demo.sarSpeedruns)
		if (result.complete && result.totalTicks > 0) ticks = result.totalTicks;
	bool hasTime = ticks > 0;
	if (!hasTime) ticks = demo.lastTick - demo.firstTick;
	float seconds = ticks * demo.intervalPerTick;
	bool coop = demo.header.map.compare(0, 8, "mp_coop_") == 0;
	bool host = demo.header.server.compare(0, 9, "localhost") == 0;
	printf("{\"ok\":true,\"map\":%s,\"coop\":%s,\"player\":%s,\"host\":%s,\"ticks\":%d,\"time\":%.4f,\"timeText\":%s,\"hasTime\":%s}\n",
	       Json(demo.header.map).c_str(), coop ? "true" : "false", Json(demo.header.client).c_str(), host ? "true" : "false", ticks, seconds,
	       Json(route::FormatTime(seconds)).c_str(), hasTime ? "true" : "false");
	return 0;
}

// Development aid: the route's samples, one line per tick and player
int Samples(const std::vector<std::string> &paths, int from, int to) {
	route::Route route;
	std::string error;
	if (!route::Load(paths, route, error)) return fprintf(stderr, "%s\n", error.c_str()), 1;
	printf("ticks %d..%d, timer %d..%d\n", route.firstTick, route.lastTick, route.timerStart, route.timerEnd);
	for (int tick = route.ClampTick(from); tick <= route.ClampTick(to); tick++)
		for (size_t a = 0; a < route.actors.size(); a++) {
			const route::Sample &s = route.At(static_cast<int>(a), tick);
			printf("%5d %-6s %s pos %9.2f %9.2f %8.2f  vel %8.1f %8.1f %8.1f  aim %6.1f %7.1f  %s%s buttons %d\n", tick,
			       route::TeamName(route.actors[a].team), s.present ? "in " : "out", s.pos.x, s.pos.y, s.pos.z, s.vel.x, s.vel.y, s.vel.z, s.pitch,
			       s.yaw, s.onGround ? "ground " : "air    ", s.ducked ? "duck" : "    ", s.buttons);
		}
	for (const auto &portal : route.portals)
		printf("portal @%d actor %d entity %d second %d %s pos %.1f %.1f %.1f ang %.1f %.1f %.1f\n", portal.tick, portal.actor, portal.portalEnt,
		       portal.portal2, portal.active ? "open" : "closed", portal.pos.x, portal.pos.y, portal.pos.z, portal.ang.x, portal.ang.y, portal.ang.z);
	for (const auto &mover : route.movers) {
		const auto &first = mover.keys.front(), &last = mover.keys.back();
		printf("object %d %s: %d keys, ticks %d..%d, from %.0f %.0f %.0f to %.0f %.0f %.0f%s\n", mover.entIndex, mover.className.c_str(),
		       static_cast<int>(mover.keys.size()), first.tick, last.tick, first.pos.x, first.pos.y, first.pos.z, last.pos.x, last.pos.y, last.pos.z,
		       last.present ? "" : " (gone)");
	}
	printf("world ticks:");
	for (int tick : route.worldTicks) printf(" %d", tick);
	printf("\n");
	for (const auto &glitch : route.buttonGlitches) printf("button glitch: tick %d, actor %d, %s\n", glitch.tick, glitch.actor, glitch.button.c_str());
	return 0;
}

int Info(const std::vector<std::string> &paths, bool text) {
	route::Route route;
	std::string error;
	if (!route::Load(paths, route, error)) return text ? (fprintf(stderr, "%s\n", error.c_str()), 1) : Fail(error);
	if (text) {
		fputs(route::Describe(route).c_str(), stdout);
		for (const auto &event : route.events) printf("  %6s  %s\n", route::FormatTime(route.TimeAt(event.tick)).c_str(), event.label.c_str());
		return 0;
	}
	std::string out = "{\"ok\":true,\"map\":" + Json(route.map) + ",\"name\":" + Json(route.name) + ",\"coop\":" + (route.coop ? "true" : "false") +
	                  ",\"challenge\":" + (route.challenge ? "true" : "false");
	char number[96];
	snprintf(number, sizeof(number), ",\"ticks\":%d,\"time\":%.4f,\"timeText\":", route.timerEnd - route.timerStart, route.RunTime());
	out += number + Json(route::FormatTime(route.RunTime())) + ",\"players\":[";
	for (size_t i = 0; i < route.actors.size(); i++) {
		const route::Actor &actor = route.actors[i];
		out += std::string(i ? "," : "") + "{\"team\":" + Json(TeamKey(actor.team)) + ",\"name\":" + Json(actor.name) + ",\"hasInputs\":" +
		       (actor.hasInputs ? "true" : "false") + "}";
	}
	out += "],\"parts\":[";
	for (size_t i = 0; i < route.segments.size(); i++) {
		const route::Segment &part = route.segments[i];
		snprintf(number, sizeof(number), "{\"index\":%d,\"start\":%.3f,\"end\":%.3f,\"name\":", static_cast<int>(i) + 1, route.TimeAt(part.startTick),
		         route.TimeAt(part.endTick));
		out += std::string(i ? "," : "") + number + Json(part.name) + "}";
	}
	out += "],\"notes\":[";
	for (size_t i = 0; i < route.notes.size(); i++) out += std::string(i ? "," : "") + Json(route.notes[i]);
	out += "]}";
	puts(out.c_str());
	return 0;
}

}  // namespace

int main(int argc, char **argv) {
	std::vector<std::string> args(argv + 1, argv + argc);
	bool text = false;
	for (size_t i = 0; i < args.size();)
		if (args[i] == "--text") {
			text = true;
			args.erase(args.begin() + i);
		} else {
			i++;
		}
	if (args.size() == 2 && args[0] == "scan") return Scan(args[1].c_str());
	if ((args.size() == 2 || args.size() == 3) && args[0] == "info") return Info({args.begin() + 1, args.end()}, text);
	if (args.size() >= 4 && args[0] == "samples")
		return Samples({args.begin() + 1, args.end() - 2}, atoi(args[args.size() - 2].c_str()), atoi(args[args.size() - 1].c_str()));
	fprintf(stderr, "usage: p2route scan <demo> | p2route info [--text] <demo> [partner demo]\n");
	return 1;
}
