// The console commands (prac_*).
#include "trainer_internal.h"
#include "use.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace trainer {

// A demo named on the console: as given, or under the game folder, or its demos folder; ".dem" may be left out.
std::string FindDemo(const std::string &name) {
	namespace fs = std::filesystem;
	std::string game = engine::GameDirectory();
	const std::string candidates[] = {name, name + ".dem", game + "/" + name, game + "/" + name + ".dem", game + "/demos/" + name,
	                                  game + "/demos/" + name + ".dem"};
	std::error_code ec;
	for (const std::string &path : candidates)
		if (fs::is_regular_file(fs::u8path(path), ec)) return path;
	return "";
}

bool LoadRoute(const sdk::CCommand &args, Route &out) {
	std::vector<std::string> paths;
	for (int i = 1; i < args.ArgC() && i <= 2; i++) {
		std::string path = FindDemo(args.Arg(i));
		if (path.empty()) {
			engine::Warn("Demo not found: %s\n", args.Arg(i));
			return false;
		}
		paths.push_back(path);
	}
	std::string error;
	if (!route::Load(paths, out, error)) {
		engine::Warn("Couldn't read the run: %s\n", error.c_str());
		return false;
	}
	return true;
}

void PrintLines(const std::string &text) {
	size_t start = 0;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos) end = text.size();
		engine::Print("%s\n", text.substr(start, end - start).c_str());
		start = end + 1;
	}
}

void CmdLoad(const sdk::CCommand &args) {
	if (args.ArgC() < 2) {
		engine::Print("prac_load <demo> [partner's demo] - load the run to practice against\n");
		return;
	}
	Route route;
	if (!LoadRoute(args, route)) return;
	Stop(false);
	g.route = std::move(route);
	g.hasRoute = true;
	g.replaying = false;
	g.bestFromTick.clear();
	g.bestPart.clear();
	g.results.clear();
	g.attempts = 0;
	g.worldReached = g.levelTouched ? g.route.lastTick : g.route.firstTick - 1;
	PrintLines(route::Describe(g.route));
	if (MapMatches()) engine::Print("Ready. prac_start [part] to practice, prac_watch [part] to ride along.\n");
	else engine::Print("Load %s to practice it (%s).\n", g.route.name.c_str(), LoadCommand().c_str());
}

void CmdInfo(const sdk::CCommand &) {
	if (!g.hasRoute) {
		engine::Print("No route loaded.\n");
		return;
	}
	PrintLines(route::Describe(g.route));
	for (const auto &event : g.route.events)
		engine::Print("  %6s  %s\n", ShowAt(event.tick).c_str(), event.label.c_str());
}

int PartArgument(const sdk::CCommand &args, int fallback) {
	return args.ArgC() >= 2 ? atoi(args.Arg(1)) - 1 : fallback;
}

constexpr const char *kNoRoute = "No route loaded. Use prac_load <demo> [partner's demo].\n";

void CmdStart(const sdk::CCommand &args) {
	if (BusyRestoring()) return;
	g.replaying = false;
	StartPart(PartArgument(args, g.hasRoute && g.attempts ? g.segment : 0), false);
}

void CmdWatch(const sdk::CCommand &args) {
	if (BusyRestoring()) return;
	g.replaying = false;
	StartPart(PartArgument(args, 0), true);
}

// The run's tick at `seconds` of its time
int TickAtSeconds(float seconds) {
	return g.route.timerStart + static_cast<int>(std::lround(seconds / g.route.tickInterval));
}

void CmdGoto(const sdk::CCommand &args) {
	if (args.ArgC() < 2) {
		engine::Print("prac_goto <seconds> [watch] - practice from that time of the run (or watch it from there)\n");
		return;
	}
	if (!g.hasRoute) {
		engine::Warn(kNoRoute);
		return;
	}
	if (BusyRestoring()) return;
	g.replaying = false;
	bool watch = args.ArgC() >= 3 && !_stricmp(args.Arg(2), "watch");
	Start(TickAtSeconds(static_cast<float>(atof(args.Arg(1)))), watch);
}

// The same attempt, taking over a little later (or earlier) in the run: for a pair of binds that move the
// handover while practicing
void CmdNudge(const sdk::CCommand &args) {
	if (args.ArgC() < 2) {
		engine::Print("prac_nudge <seconds> - take over this many seconds later in the run (negative: earlier) and start again\n");
		return;
	}
	if (!g.hasRoute) {
		engine::Warn(kNoRoute);
		return;
	}
	if (BusyRestoring()) return;
	int from = g.attempts ? g.handover : g.route.segments[0].startTick;
	g.replaying = false;
	Start(from + static_cast<int>(std::lround(atof(args.Arg(1)) / g.route.tickInterval)), g.watch);
}

void CmdRetry(const sdk::CCommand &) {
	if (g.hasRoute) Start(g.attempts ? g.handover : g.route.segments[0].startTick, g.watch);
	else engine::Warn(kNoRoute);
}

void CmdNext(const sdk::CCommand &) {
	if (BusyRestoring()) return;
	g.replaying = false;
	StartPart(g.segment + 1, false);
}

void CmdPrev(const sdk::CCommand &) {
	if (BusyRestoring()) return;
	g.replaying = false;
	StartPart(g.segment - 1, false);
}

void CmdStop(const sdk::CCommand &) {
	Stop(true);
}

void CmdAuto(const sdk::CCommand &args) {
	std::string first = args.ArgC() >= 2 ? args.Arg(1) : "";
	if (first.empty() || first == "off") {
		g.autoPart = -1;
		g.autoSeconds = -1;
		engine::Print("prac_auto [watch] <part> | [watch] at <seconds> | off - start by itself when the route's map is up (now: off)\n");
		return;
	}
	g.autoWatch = first == "watch";
	int where = g.autoWatch ? 2 : 1;  // the argument that says where: a part number, or "at" and a time
	std::string what = args.ArgC() > where ? args.Arg(where) : "";
	bool bySeconds = what == "at";
	float seconds = bySeconds && args.ArgC() > where + 1 ? static_cast<float>(atof(args.Arg(where + 1))) : 0;
	g.autoPart = bySeconds ? 0 : std::max(0, atoi(what.c_str()) - 1);
	g.autoSeconds = bySeconds ? std::max(0.0f, seconds) : -1;
	g.waitTicks = 0;
	if (bySeconds) engine::Print("Will %s from %.2f s of the run as soon as the map is running.\n", g.autoWatch ? "watch" : "practice", g.autoSeconds);
	else engine::Print("Will %s from part %d as soon as the map is running.\n", g.autoWatch ? "watch" : "practice", g.autoPart + 1);
}

void CmdReplay(const sdk::CCommand &args) {
	if (args.ArgC() < 2 || !g.hasRoute) {
		engine::Print("prac_replay <demo> [partner's demo] - play another run in the level against the loaded one\n");
		return;
	}
	if (BusyRestoring()) return;
	Route other;
	if (!LoadRoute(args, other)) return;
	if (other.map != g.route.map) {
		engine::Warn("That run is on %s, the loaded one on %s.\n", other.name.c_str(), g.route.name.c_str());
		return;
	}
	g.replay = std::move(other);
	g.replaying = true;
	engine::Print("Replaying %s (%s) against the loaded run (%s).\n", g.replay.files.empty() ? "run" : g.replay.files[0].c_str(),
	              g.replay.Show(g.replay.timerEnd - g.replay.timerStart).c_str(), ShowAt(g.route.timerEnd).c_str());
	// The players are driven the whole way, so the "handover" is the end; the lead-in starts before the first part
	g.worldReached = g.route.lastTick;  // unknown relation to this run's world: start from a fresh level
	Start(g.route.segments[0].startTick, false);
}

// Prints, part by part, where another run gains or loses against the loaded one. Needs no game: it compares the
// two recordings.
void CmdCompare(const sdk::CCommand &args) {
	if (args.ArgC() < 2 || !g.hasRoute) {
		engine::Print("prac_compare <demo> [partner's demo] - part-by-part times of another run against the loaded one\n");
		return;
	}
	Route other;
	if (!LoadRoute(args, other)) return;
	if (other.map != g.route.map) {
		engine::Warn("That run is on %s, the loaded one on %s.\n", other.name.c_str(), g.route.name.c_str());
		return;
	}
	const float dt = g.route.tickInterval;
	engine::Print("%-5s %-44s %8s %8s %8s\n", "part", "", "record", "this run", "diff");
	int previous = other.timerStart;
	int previousRecord = g.route.timerStart;
	for (size_t i = 0; i < g.route.segments.size(); i++) {
		const route::Segment &part = g.route.segments[i];
		int reached = other.timerEnd;
		if (i + 1 < g.route.segments.size()) {
			// When the other run's players are closest to where the record's players are at this part's end
			int window = std::min(other.timerEnd, previous + 3 * (part.endTick - previousRecord) + 180);
			float best = 1e30f;
			for (int tick = previous; tick <= window; tick++) {
				float total = 0;
				for (size_t a = 0; a < g.route.actors.size(); a++) {
					int actor = other.ActorForTeam(g.route.actors[a].team);
					if (actor < 0) continue;
					total += Distance(other.At(actor, tick).pos, g.route.At(static_cast<int>(a), part.endTick).pos);
				}
				if (total < best) {
					best = total;
					reached = tick;
				}
			}
		}
		int record = part.endTick - previousRecord, mine = reached - previous;
		engine::Print("%-5d %-44.44s %8s %8s %8s\n", static_cast<int>(i) + 1, part.name.c_str(), route::FormatHundredths(route::Hundredths(record)).c_str(),
		              route::FormatHundredths(route::Hundredths(mine)).c_str(), Gap(mine, record).c_str());
		previous = reached;
		previousRecord = part.endTick;
	}
	engine::Print("%-5s %-44s %8s %8s %8s\n", "total", "", ShowAt(g.route.timerEnd).c_str(), other.Show(other.timerEnd - other.timerStart).c_str(),
	              Gap(other.timerEnd - other.timerStart, g.route.timerEnd - g.route.timerStart).c_str());
}

void CmdRole(const sdk::CCommand &args) {
	std::string role = args.ArgC() >= 2 ? args.Arg(1) : "";
	if (role == "blue" || role == "atlas") g_set.role = route::kTeamBlue;
	else if (role == "orange" || role == "pbody" || role == "p-body") g_set.role = route::kTeamOrange;
	else if (role == "auto") g_set.role = 0;
	else {
		engine::Print("prac_role <blue|orange|auto> - the robot you play when practicing co-op alone (now: %s)\n",
		              g_set.role == route::kTeamBlue ? "blue" : g_set.role == route::kTeamOrange ? "orange" : "auto");
		return;
	}
	engine::Print("You play %s; the partner bot plays the other robot.\n", g_set.role ? route::TeamName(g_set.role) : "the robot you spawn as");
}

void PrintSettings() {
	static const char *const resets[] = {"auto", "soft", "reload"};
	engine::Print("prac_set <name> <value>:\n");
	engine::Print("  leadin %.1f      seconds the run plays itself before you take over\n", g_set.leadIn);
	engine::Print("  ghost %d         show the recorded players\n", g_set.ghost);
	engine::Print("  ghoststyle %s  how: basic (wireframe, the large shapes), wirelow (the coarse mesh), wire (the full mesh) or solid\n",
	              g_set.ghostStyle == kGhostSolid ? "solid" : g_set.ghostStyle == kGhostWire ? "wire" : g_set.ghostStyle == kGhostWireLow ? "wirelow" : "basic");
	engine::Print("  hud %d           the clock, the gap to the record and the flashes on screen\n", g_set.hud);
	engine::Print("  speedometer %d   your speed next to the record's, at the bottom left\n", g_set.speedometer);
	engine::Print("  strafe %d        bhop trainer: speed gained per jump, air time spent gaining, hop timing\n", g_set.strafe);
	engine::Print("  use %d           use trainer: how far off, and how early or late, a use key press that used nothing was\n", g_set.use);
	engine::Print("  path %d          draw the route and portal shots in the level (host only)\n", g_set.path);
	engine::Print("  partner %d       alone in co-op: a bot plays the other robot's half\n", g_set.partner);
	engine::Print("  loop %d          start again by itself after the finish\n", g_set.loop);
	engine::Print("  autoreset %.1f   retry by itself when this many seconds behind (0 = off)\n", g_set.autoReset);
	engine::Print("  speed %.2f       game speed while practicing\n", g_set.speed);
	engine::Print("  reset %s       back to a part's start: auto, soft (teleport only) or reload (restart the level)\n", resets[g_set.reset]);
	engine::Print("  challenge %d     practice every run in Challenge Mode (the flags end it); 0: only runs made in it\n", g_set.challenge);
	if (g_set.ghostColorSet) engine::Print("  ghostcolor %d %d %d  the ghosts' colour (red green blue), or team for their robots' colours\n", g_set.ghostColor[0], g_set.ghostColor[1], g_set.ghostColor[2]);
	else engine::Print("  ghostcolor team   the ghosts' colour: their robots' colours, or three numbers 0-255 (red green blue)\n");
	engine::Print("  ghostopacity %d  how solid the ghosts are drawn, in percent\n", g_set.ghostOpacity);
	engine::Print("  snapshots %d     retry instantly through ChocolateLoading's snapshots when it is running (experimental)\n", g_set.snapshots);
}

void CmdSet(const sdk::CCommand &args) {
	if (args.ArgC() < 3) {
		PrintSettings();
		return;
	}
	std::string name = args.Arg(1), value = args.Arg(2);
	float number = static_cast<float>(atof(value.c_str()));
	bool on = number != 0;
	if (name == "leadin") g_set.leadIn = std::clamp(number, 0.0f, 10.0f);
	else if (name == "ghost") g_set.ghost = on;
	else if (name == "ghoststyle") g_set.ghostStyle = value == "solid" ? kGhostSolid : value == "wire" ? kGhostWire : value == "wirelow" ? kGhostWireLow : kGhostBasic;
	else if (name == "hud") g_set.hud = on;
	else if (name == "speedometer") g_set.speedometer = on;
	else if (name == "strafe") g_set.strafe = on;
	else if (name == "use") g_set.use = on;
	else if (name == "path") {
		g_set.path = on;
		// (in an attempt the line comes or goes at once)
		if (g.mode == Mode::LeadIn || g.mode == Mode::Run || g.mode == Mode::Watch) SetUpRoute();
	}
	else if (name == "partner") g_set.partner = on;
	else if (name == "loop") g_set.loop = on;
	else if (name == "autoreset") g_set.autoReset = std::max(0.0f, number);
	else if (name == "speed") g_set.speed = std::clamp(number, 0.05f, 4.0f);
	else if (name == "reset") g_set.reset = value == "soft" ? kResetSoft : value == "reload" ? kResetReload : kResetAuto;
	else if (name == "challenge") g_set.challenge = on;
	else if (name == "ghostcolor") {
		// "team", or three numbers 0-255 (as one quoted value or three arguments)
		std::string text = value;
		for (int i = 3; i < args.ArgC(); i++) text += std::string(" ") + args.Arg(i);
		int r, gg, b;
		if (text == "team") g_set.ghostColorSet = false;
		else if (sscanf(text.c_str(), "%d %d %d", &r, &gg, &b) == 3) {
			g_set.ghostColorSet = true;
			g_set.ghostColor[0] = std::clamp(r, 0, 255);
			g_set.ghostColor[1] = std::clamp(gg, 0, 255);
			g_set.ghostColor[2] = std::clamp(b, 0, 255);
		} else {
			engine::Warn("ghostcolor takes \"team\" or three numbers 0-255 (red green blue).\n");
			return;
		}
		if (g.helperLoaded) RecolorGhosts();
	}
	else if (name == "ghostopacity") {
		g_set.ghostOpacity = std::clamp(static_cast<int>(number), 0, 100);
		if (g.helperLoaded) RecolorGhosts();
	}
	else if (name == "snapshots") {
		g_set.snapshots = on;
		// (one being stored or brought back just now is left to finish: the next attempt does without)
		if (!g.save.Busy()) DropSnapshot();
		if (on && !engine::CommandExists("snap_save"))
			engine::Print("ChocolateLoading isn't running (no snap_save command): retries will restart the level as usual.\n");
		else if (on && !SnapshotsReady())
			engine::Print("This ChocolateLoading can't be used for retries: they need the build that comes with P2 Practice, with "
			              "its \"snap_opt render_lock\" left on. Retries will restart the level as usual.\n");
	}
	else {
		engine::Warn("No setting named %s.\n", name.c_str());
		PrintSettings();
	}
}

// Development aid: what the plugin reads from the game right now
// The development switches of prac_debug answer only after "prac_debug dev 1" (the map tests and the dev
// scripts send it); "prac_debug" alone (the status, for bug reports) always works.
bool g_devCommands = false;

// From the helper script: the entity number of the ghost it made for a robot
void CmdGhostEntity(const sdk::CCommand &args) {
	size_t actor = static_cast<size_t>(atoi(args.Arg(1)));
	if (args.ArgC() >= 3 && actor < g.ghosts.size()) g.ghosts[actor].entIndex = atoi(args.Arg(2));
}

void CmdDebug(const sdk::CCommand &args) {
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "dev")) {
		g_devCommands = atoi(args.Arg(2)) != 0;
		return;
	}
	if (args.ArgC() >= 2 && !g_devCommands) {
		engine::Print("prac_debug - the plugin's state, for bug reports\n");
		return;
	}
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "exec")) {
		// (development: a console line run as the player, from a console that has no player of its own)
		engine::Execute("%s", args.ArgS() + strlen("exec "));
		return;
	}
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "noghostmove")) {
		g_noGhostMove = atoi(args.Arg(2)) != 0;
		return;
	}
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "nopose")) {
		g_noPose = atoi(args.Arg(2)) != 0;
		return;
	}
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "follow")) {
		g_followTicks = std::max(0, atoi(args.Arg(2)));
		return;
	}
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "decimals")) {
		g_positionDecimals = std::clamp(atoi(args.Arg(2)), 0, 6);
		return;
	}
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "pickups")) {
		g_replayPickups = atoi(args.Arg(2)) != 0;
		return;
	}
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "botmove")) {
		g_botPlacedOnly = atoi(args.Arg(2)) == 0;
		return;
	}
	// prac_debug result <part> <seconds> [flags]: a finished attempt as if played, for the menu's session panel
	if (args.ArgC() >= 4 && !strcmp(args.Arg(1), "result") && g.hasRoute) {
		Result result;
		result.part = std::clamp(atoi(args.Arg(2)) - 1, 0, static_cast<int>(g.route.segments.size()) - 1);
		result.seconds = static_cast<float>(atof(args.Arg(3)));
		result.gap = result.seconds - (g.route.timerEnd - g.route.timerStart) * g.route.tickInterval;
		result.flags = args.ArgC() >= 5 && atoi(args.Arg(4)) != 0;
		int ticks = static_cast<int>(std::lround(result.seconds / g.route.tickInterval));
		RecordBest(g.bestFromTick, g.route.segments[result.part].startTick, ticks, result.best);
		g.results.push_back(result);
		g.attempts++;
		return;
	}
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "portallead")) {
		g_portalLead = std::clamp(atoi(args.Arg(2)), 0, 6);
		return;
	}
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "snapdelay")) {
		g_snapshotDelayTicks = std::max(1, atoi(args.Arg(2)));
		return;
	}
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "edicts")) {
		g_scanEdicts = atoi(args.Arg(2)) != 0;
		g_lastBadEdict = -1;
		for (size_t a = 0; a < g.ghosts.size(); a++) engine::Print("ghost %d: entity %d\n", static_cast<int>(a), g.ghosts[a].entIndex);
		ScanEdicts(engine::TickCount(), false);
		return;
	}
	if (args.ArgC() >= 5 && !strcmp(args.Arg(1), "useat")) {
		use::PrintProbe(static_cast<float>(atof(args.Arg(2))), static_cast<float>(atof(args.Arg(3))), static_cast<float>(atof(args.Arg(4))));
		return;
	}
	if (args.ArgC() >= 2 && !strcmp(args.Arg(1), "use")) {
		use::PrintState();
		return;
	}
	if (args.ArgC() >= 2 && !strcmp(args.Arg(1), "perf")) {
		PrintPerf();
		return;
	}
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "arrivals")) {
		g_logArrivals = atoi(args.Arg(2)) != 0;
		return;
	}
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "parity")) {
		g_frameStartParity = atoi(args.Arg(2)) & 1;
		return;
	}
	if (args.ArgC() >= 3 && !strcmp(args.Arg(1), "record")) {
		// (for the map tests: the demo of the next attempt, begun the moment the attempt does, after any reload)
		g.recordNext = args.Arg(2);
		return;
	}
	if (args.ArgC() >= 2 && !strcmp(args.Arg(1), "messages")) {
		engine::PrintUserMessages();
		if (args.ArgC() >= 3) engine::Print("sent: %d\n", engine::SendScoreboard(-1, atoi(args.Arg(2))));
		if (args.ArgC() >= 4) engine::Print("chat sent: %d\n", engine::SendTestChat(args.Arg(3)));
		return;
	}
	if (args.ArgC() >= 2 && !strcmp(args.Arg(1), "raw")) {
		engine::PrintRawState();
		return;
	}
	static const char *const modes[] = {"idle", "reloading", "waiting for players", "catching up", "lead-in", "run", "watch", "done", "going back"};
	engine::Print("map %s, tick %d, mode %s, cursor %d, handover %d, world reached %d\n", engine::MapName(), engine::TickCount(),
	              modes[static_cast<int>(g.mode)], g.cursor, g.handover, g.worldReached);
	for (const LivePlayer &p : LivePlayers())
		engine::Print("player %d: team %d %s%s pos %.1f %.1f %.1f vel %.1f %.1f %.1f aim %.1f %.1f flags %d\n", p.entIndex, p.state.team,
		              p.state.bot ? "bot" : "person", p.state.alive ? "" : " (dead)", p.state.origin.x, p.state.origin.y, p.state.origin.z,
		              p.state.velocity.x, p.state.velocity.y, p.state.velocity.z, p.state.pitch, p.state.yaw, p.state.flags);
	for (size_t a = 0; a < g.slots.size(); a++)
		engine::Print("slot %d (%s): player %d %s progress %d part %d%s\n", static_cast<int>(a), route::TeamName(g.route.actors[a].team),
		              g.slots[a].entIndex, g.slots[a].human ? "person" : "driven", g.slots[a].progress, g.slots[a].part + 1,
		              g.slots[a].finished ? " finished" : "");
}

void CmdHelp(const sdk::CCommand &) {
	engine::Print("P2 Practice: practice against a demo.\n");
	engine::Print("  prac_load <demo> [partner's demo]   the run to practice against\n");
	engine::Print("  prac_info                           its parts and events\n");
	engine::Print("  prac_start [part]                   practice from a part (the run plays itself up to it, then you take over)\n");
	engine::Print("  prac_goto <seconds>                 practice from any time of the run\n");
	engine::Print("  prac_nudge <seconds>                take over this many seconds later in the run (negative: earlier)\n");
	engine::Print("  prac_retry / prac_next / prac_prev  again, next part, previous part\n");
	engine::Print("  prac_watch [part]                   ride along with the recorded run\n");
	engine::Print("  prac_role <blue|orange|auto>        co-op alone: which robot you play; a bot plays the other\n");
	engine::Print("  prac_replay <demo> [partner's]      play another run in the level against the loaded one\n");
	engine::Print("  prac_compare <demo> [partner's]     part-by-part times of another run against the loaded one\n");
	engine::Print("  prac_set [name value]               settings\n");
	engine::Print("  prac_auto [watch] <part> | at <s>   start by itself when the route's map is up (the launcher uses this)\n");
	engine::Print("  prac_stop                           stop\n");
}

const std::vector<Command> kCommands = {
	{"prac_load", CmdLoad, "prac_load <demo> [partner's demo] - load the run to practice against", false},
	{"prac_info", CmdInfo, "prac_info - the loaded run's parts and events", true},
	{"prac_start", CmdStart, "prac_start [part] - practice from a part of the run", true},
	{"prac_goto", CmdGoto, "prac_goto <seconds> - practice from a time of the run", true},
	{"prac_nudge", CmdNudge, "prac_nudge <seconds> - take over this many seconds later in the run (negative: earlier)", true},
	{"prac_retry", CmdRetry, "prac_retry - the same attempt again", true},
	{"prac_next", CmdNext, "prac_next - practice the next part", true},
	{"prac_prev", CmdPrev, "prac_prev - practice the previous part", true},
	{"prac_watch", CmdWatch, "prac_watch [part] - ride along with the recorded run", true},
	{"prac_stop", CmdStop, "prac_stop - stop practicing", true},
	{"prac_auto", CmdAuto, "prac_auto [watch] <part> | [watch] at <seconds> | off - start by itself when the route's map is up", false},
	{"prac_role", CmdRole, "prac_role <blue|orange|auto> - the robot you play when alone in co-op", false},
	{"prac_replay", CmdReplay, "prac_replay <demo> [partner's demo] - play another run against the loaded one", false},
	{"prac_compare", CmdCompare, "prac_compare <demo> [partner's demo] - part-by-part comparison with the loaded run", false},
	{"prac_set", CmdSet, "prac_set [name value] - settings", false},
	{"prac_help", CmdHelp, "prac_help - list the practice commands", true},
	{"prac_debug", CmdDebug, "prac_debug - print what the plugin reads from the game (for bug reports)", false},
};

}  // namespace trainer
