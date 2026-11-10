// Moving the game along the run: players, portals, objects and keys.
#include "trainer_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace trainer {

// Portals the trainer opened through the console because nobody in the game owns them (a robot nobody plays):
// where they are, so they can be closed again.
std::vector<Vec3> g_orphanPortals;

// The live portal a recorded portal change applies to: one of the two portals of the player who stands in for
// the robot that shot it. 0 when nobody plays that robot (or the map itself placed the portal).
int LivePortal(const route::PortalChange &change, const std::vector<int> &playerOfActor) {
	if (change.actor < 0 || change.actor >= static_cast<int>(playerOfActor.size()) || !playerOfActor[change.actor]) return 0;
	int primary, secondary;
	if (!engine::GetPlayerPortals(playerOfActor[change.actor], primary, secondary)) return 0;
	int portal = change.portal2 ? secondary : primary;
	return portal > 0 ? portal : 0;
}

// `open`: the portal is open already (its previous change in the run opened it), so it is only moved.
// `at`: the run's tick the level is at. A portal on a moving panel goes where the run had it at that tick; the
// game then fastens it to the panel behind it, as it does for a shot.
void ApplyPortalChange(const Route &route, const route::PortalChange &placed, int at, bool open = false) {
	const std::vector<int> players = PlayersOf(route);
	route::PortalChange change = placed;
	change.pos = placed.PosAt(at);
	int team = change.actor >= 0 ? route.actors[change.actor].team : 0;
	if (int portal = LivePortal(change, players)) {
		// the player's own portal: moved as if they had shot it there, so their next shot pairs with it
		if (change.active)
			engine::Script("::PP.PortalMove(%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d)", portal, change.pos.x, change.pos.y, change.pos.z, change.ang.x,
			               change.ang.y, change.ang.z, team, open ? 1 : 0);
		else
			engine::Script("::PP.PortalClose(%d)", portal);
		return;
	}
	if (change.actor < 0 && !route.coop) return;  // single player: a portal the map itself opens; the map will
	if (change.active) {
		// No live portal to move. In single player that is the player's own pair before its first shot (the gun
		// makes its portals on demand; they are pair 0). In co-op it is the pair of a robot nobody plays: it gets a
		// pair of its own. Either way the console command places it.
		int pair = route.coop ? 40 + std::max(0, change.actor) : 0;
		char args[192];
		snprintf(args, sizeof(args), "%d %d %.3f %.3f %.3f %.3f %.3f %.3f", pair, change.portal2 ? 1 : 0, change.pos.x,
		         change.pos.y, change.pos.z, change.ang.x, change.ang.y, change.ang.z);
		engine::CallCommand("portal_place", args);
		if (team) engine::Script("::PP.PortalTeam(%.1f,%.1f,%.1f,%d)", change.pos.x, change.pos.y, change.pos.z, team);
		if (route.coop) g_orphanPortals.push_back(change.pos);
		return;
	}
	// Closing: the portal is where its previous change left it
	for (auto it = route.portals.rbegin(); it != route.portals.rend(); ++it)
		if (it->tick < change.tick && it->portalEnt == change.portalEnt && it->active) {
			engine::Script("::PP.ClosePortalAt(%.1f,%.1f,%.1f)", it->pos.x, it->pos.y, it->pos.z);
			break;
		}
}

// Opens the portals the run had open at `tick` and closes the others the trainer is responsible for: the
// players' own pairs and the ones it placed itself. Portals the map placed are left alone.
int g_portalsPlacedTo = -1;  // the run's portal changes up to this tick are in the level (see PlacePortals)

void SetPortals(const Route &route, int tick) {
	g_portalsPlacedTo = tick;
	std::map<int, const route::PortalChange *> state;  // by demo portal entity
	for (const auto &change : route.portals)
		if (change.tick <= tick) state[change.portalEnt] = &change;
	std::vector<int> open;        // live portals that stay open
	std::vector<Vec3> placedAgain;  // where a portal nobody owns is placed (again) below
	const std::vector<int> players = PlayersOf(route);
	for (const auto &[entity, change] : state) {
		if (!change->active) continue;
		if (int portal = LivePortal(*change, players)) open.push_back(portal);
		else placedAgain.push_back(change->PosAt(tick));
	}
	// The portals the trainer placed for an earlier attempt go, except one that is placed where it is again: the
	// console command places at once and the script's close runs after it, so it would close the new portal.
	constexpr float kSamePortal = 16;  // (the reach of ::PP.ClosePortalAt)
	for (const Vec3 &pos : g_orphanPortals) {
		bool again = std::any_of(placedAgain.begin(), placedAgain.end(), [&](const Vec3 &p) { return Distance(pos, p) < kSamePortal; });
		if (!again) engine::Script("::PP.ClosePortalAt(%.1f,%.1f,%.1f)", pos.x, pos.y, pos.z);
	}
	g_orphanPortals.clear();
	for (int player : players) {
		int pair[2];
		if (!player || !engine::GetPlayerPortals(player, pair[0], pair[1])) continue;
		for (int portal : pair)
			if (portal > 0 && std::find(open.begin(), open.end(), portal) == open.end()) engine::Script("::PP.PortalClose(%d)", portal);
	}
	// (script statements run in the order given: the closes above come before these moves)
	for (const auto &[entity, change] : state)
		if (change->active) ApplyPortalChange(route, *change, tick);
}

// Makes a player let go of what is in their hands (from an earlier attempt), the way a player does: by pressing
// the use key. (Removing the game's pick-up controller instead leaves the object floating without gravity.) The
// key is pressed with a console command: "+use" for the player at this computer, "cmd2 +use" for the second
// split-screen player, and released on the next tick. A partner on another computer has no key the host can
// press: for them the controller is removed, which is the best that can be done from here.
//
// The game acts on the key going down, so after a press the key stays up for a whole tick before the next one: a
// release and a new press within one tick (asked for again on the tick after, the object not yet let go) cancel
// out, and the key would look held for good - a bot kept a cube for a second that way in Bridge Testing.
void DropCarried(const Slot &slot) {
	engine::PlayerState state;
	if (!engine::GetPlayer(slot.entIndex, state) || state.held < 0) return;
	auto pressed = g.usePressedAt.find(slot.entIndex);
	// (the tick count starts again with each level: an older press from before a load doesn't count)
	if (pressed != g.usePressedAt.end() && engine::TickCount() >= pressed->second && engine::TickCount() - pressed->second < 2) return;
	if (slot.entIndex == 1) engine::Execute("+use");
	else if (state.bot) engine::Execute("cmd2 +use");
	else {
		engine::Script("::PP.Drop(%d)", slot.entIndex);
		return;
	}
	g.useKeyDown.push_back(slot.entIndex);
	g.usePressedAt[slot.entIndex] = engine::TickCount();
}

// Releases the use keys pressed on the previous tick (and the jump key: see PressJump).
void ReleaseUseKeys() {
	for (int entIndex : g.useKeyDown) engine::Execute(entIndex == 1 ? "-use" : "cmd2 -use");
	g.useKeyDown.clear();
	if (g.jumpKeyDown) engine::Execute("-jump");
	g.jumpKeyDown = false;
}

// The duck key of the player at this computer, held while the run's player crouched (they fit where it did, and
// what they carry hangs as low). Let go whenever nobody is driven any more: the person's own key is theirs.
void SetDuckKey(bool down) {
	if (down == g.duckKeyDown) return;
	engine::Execute(down ? "+duck" : "-duck");
	g.duckKeyDown = down;
}

// A driven player at this computer presses jump where the run's player did. Moving the player is done by
// placing it, but a level may listen for the key itself: Container Ride's opening waits for the player to press
// space (logic_playerproxy OnJump, on every press, in the air too), and without the presses the container ride
// never starts. (Only the recording player's keys are in a demo, and only the player at this computer can be given
// a key: a bot moves by being placed, with its own movement off.)
void PressJump(const Slot &slot, const Sample &now, const Sample &before) {
	if (slot.entIndex != 1 || now.buttons < 0 || !(now.buttons & p2demo::IN_JUMP)) return;
	if (before.buttons >= 0 && (before.buttons & p2demo::IN_JUMP)) return;
	engine::Execute("+jump");
	g.jumpKeyDown = true;
}

// Things a player presses, as opposed to things a player picks up: buttons (func_button, prop_button,
// prop_under_button, func_rot_button) and the co-op levers, whose knob is a func_door_rotating used by the player
// (Behind the Scenes, Bridge Testing, Industrial Fan)
bool IsButton(const std::string &className) {
	return className.find("button") != std::string::npos || className == "func_door_rotating";
}

// The direction a recorded player looks in
Vec3 AimDirection(const Sample &s) {
	const float kToRadians = 3.14159265f / 180;
	float pitch = s.pitch * kToRadians, yaw = s.yaw * kToRadians;
	return {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
}

bool g_botPlacedOnly = true;  // development: "prac_debug botmove 1" lets the game move the bot between placements again
bool g_replayPickups = true;  // development: "prac_debug pickups 0" leaves the run's pick-ups out (see DriveSlots)

constexpr float kStandEyeHeight = 64, kDuckEyeHeight = 28;  // a robot's eyes above its feet
constexpr int kFlagDucking = 2;                              // FL_DUCKING

bool BotPlacedOnly(const Slot &slot) {
	return g_botPlacedOnly && !slot.human && !g.replaying && g.mode != Mode::CatchUp;
}

// Whether the run's player went through a portal between the tick before and this one (moved farther than anyone
// runs or flies in a tick)
bool PortalTrip(const Route &driver, int actor, int tick) {
	if (tick <= driver.firstTick) return false;
	const Sample &before = driver.At(actor, tick - 1), &now = driver.At(actor, tick);
	constexpr float kFastest = 3500;  // u/s: the game's speed limit
	return before.present && now.present && Distance(before.pos, now.pos) > 64 + kFastest * driver.tickInterval;
}

// The game's own movement for each bot: off for those the trainer placed this tick, on for the rest (practice
// over, a person in control of it, catching up)
void SetBotMovement() {
	for (int i = 1; i <= engine::MaxClients(); i++) {
		engine::PlayerState state;
		if (!engine::GetPlayer(i, state) || !state.bot) continue;
		bool placed = std::find(g.placedBots.begin(), g.placedBots.end(), i) != g.placedBots.end();
		engine::SetMovementSpeed(i, placed ? 0.0f : 1.0f);
		// (a bot moving by itself again stands up: see DriveSlots)
		if (!placed && !(state.flags & kFlagDucking)) engine::SetEyeHeight(i, kStandEyeHeight);
	}
}

void DriveSlots() {
	ReleaseUseKeys();
	const Route &driver = Driver();
	int tick = DriverTick(g.cursor);
	bool duck = false;  // (the duck key of the player at this computer: see SetDuckKey)
	struct DuckKey {
		bool &wanted;
		~DuckKey() { SetDuckKey(wanted); }
	} duckKey{duck};
	if (tick > driver.lastTick) return;
	for (size_t i = 0; i < g.slots.size(); i++) {
		if (!Driven(g.slots[i])) continue;
		int actor = DriverActor(i);
		if (actor < 0) continue;
		const Sample &s = driver.At(actor, tick);
		if (!s.present) continue;
		int player = g.slots[i].entIndex;
		// A driven player is not put where the run's player was at every tick. It is put where that player was
		// when the game's frame began, with that player's speed, and the game's own movement then carries it
		// through the frame as it carried the run's player. So it touches what that player touched, in the same
		// frame and the same order: a button pressed and released within one frame (which leaves its door open
		// for good), a trigger skipped at speed, a trip through a portal. Where no frame follows this tick, it
		// is put where the run's player was now, which is where the next frame begins.
		//
		// Not so the bot partner once a person plays beside it. The bot is a split-screen player of the person's own
		// game, which works out on screen where the game's movement takes it from one frame to the next, and its
		// guess (the bot pressing nothing) was corrected by the trainer's placement every tick: orange jittered on
		// the person's screen. The game's movement is switched off for the bot instead and it is put exactly where
		// the run's player was at every tick; the triggers it stands in still notice it (the game checks them after
		// every move, whatever the distance). (Not while catching up, where the game skips through the run.)
		//
		// A placed bot goes through a portal by being put on the far side: what it carries would be dragged
		// straight across the level after it, through whatever is in between (Catapult Catch's fizzler). It lets
		// go a few ticks before (the use key acts only on the bot's next move, and a single tick of being dragged
		// at that speed is enough to lose the object), and the trainer moves the object along the run's path
		// through the portal (MoveObjects), as it moves everything nobody holds.
		//
		// Near the flags, the host's partner of a co-op run is put where the host's game had it (Actor::seenPos),
		// in attempts and replays alike, so that it touches them on the tick the run's player did.
		Vec3 seen;
		if (driver.actors[actor].SeenAt(tick, driver.firstTick, seen)) {
			Sample where = s;
			where.pos = seen;
			MovePlayer(player, where, {});
			engine::SetEyeHeight(player, s.ducked ? kDuckEyeHeight : kStandEyeHeight);
			g.placedBots.push_back(player);
		} else if (BotPlacedOnly(g.slots[i])) {
			constexpr int kDropBeforeTrip = 4;
			for (int ahead = 1; ahead <= kDropBeforeTrip && tick + ahead <= driver.lastTick; ahead++)
				if (PortalTrip(driver, actor, tick + ahead)) {
					DropCarried(g.slots[i]);
					break;
				}
			// (Leaving the ground, the demo shows the run's player standing and barely rising for a tick or two
			// more, and the game, finding the bot a few units above the floor and not rising fast, would put it
			// back on the floor: it is given the rise it really makes.)
			Vec3 velocity = VelocityAt(driver, actor, tick);
			const Sample &previous = driver.At(actor, tick - 1);
			if (previous.present && tick > driver.firstTick) {
				float rise = (s.pos.z - previous.pos.z) / driver.tickInterval;
				if (rise > velocity.z && rise < 64 / driver.tickInterval) velocity.z = rise;
			}
			MovePlayer(player, s, velocity);
			// (crouched where the run's player crouched, as far as its eyes go: what it carries hangs in front of
			// them, and Separation's cube, carried crouching onto its button, otherwise came in 36 units too high)
			engine::SetEyeHeight(player, s.ducked ? kDuckEyeHeight : kStandEyeHeight);
			g.placedBots.push_back(player);
		} else {
			int from = FrameStartsNow() && tick > driver.firstTick ? tick - 1 : tick;
			const Sample &start = driver.At(actor, from);
			MovePlayer(player, start.present ? start : s, VelocityAt(driver, actor, start.present ? from : tick));
		}

		// The buttons the recorded player pressed on this tick, the driven one presses too. What the recorded player
		// picked up, the driven one picks up: a level may wait for that (Ricochet's cube, whose pickup opens the
		// way out); where it goes from there is still the run's path (MoveObjects), and the driven player lets go
		// when the run's did (below).
		const Sample &before = driver.At(actor, tick - 1);
		Vec3 aim = AimDirection(s);
		if (driver.hasUses) {
			for (const route::UseEvent &use : driver.uses) {
				if (use.tick != tick || use.actor != actor) continue;
				if (!IsButton(use.className)) {
					if (!g_replayPickups) continue;
					// (a pick-up, not a drop: the run's player holds something a moment later, the driven one nothing yet)
					engine::PlayerState live;
					const Sample &after = driver.At(actor, tick + 2);
					if (after.held >= 0 && engine::GetPlayer(player, live) && live.held < 0)
						engine::Script("::PP.PickUp(%d,%s,%s,%.4f,%.4f,%.4f)", player, Quote(use.className).c_str(), Quote(use.target).c_str(), aim.x, aim.y, aim.z);
					continue;
				}
				// (with everyone's portals: a player presses buttons through them)
				int portals[4] = {-1, -1, -1, -1};
				size_t pairs = 0;
				for (const Slot &slot : g.slots)
					if (slot.entIndex && pairs < 2 && engine::GetPlayerPortals(slot.entIndex, portals[pairs * 2], portals[pairs * 2 + 1])) pairs++;
				engine::Script("::PP.UseEnt(%d,%s,%s,%.4f,%.4f,%.4f,%d,%d,%d,%d)", player, Quote(use.className).c_str(), Quote(use.target).c_str(), aim.x, aim.y,
				               aim.z, portals[0], portals[1], portals[2], portals[3]);
			}
		} else if (s.buttons >= 0 && (s.buttons & p2demo::IN_USE) && !(before.buttons >= 0 && (before.buttons & p2demo::IN_USE))) {
			// no record of what was used: a button in front of them, if there is one
			engine::Script("::PP.Use(%d,%.4f,%.4f,%.4f)", player, aim.x, aim.y, aim.z);
		}
		PressJump(g.slots[i], s, before);
		if (player == 1 && !BotPlacedOnly(g.slots[i])) duck = s.ducked;
		// Hands. In co-op the game makes a player who flies fast catch what they are looking at, without any key:
		// it did so for the run's player, and it does so for the driven one. While the run's player carries
		// something that is as it should be; as soon as they don't, the driven player lets go.
		engine::PlayerState live;
		if (s.held < 0 && engine::GetPlayer(player, live) && live.held > 0) DropCarried(g.slots[i]);

		// A floor button the run's player pressed and released within one tick stays "pressed" for the map. The
		// driven player passes over it as the run's did, but at catch-up speed the game's frames fall differently
		// and the press and the release come apart. So the button is given both at once, as the run gave it, a
		// moment later (when the driven player has stepped off): the same outputs in the same order. (Where the
		// driven player already did it, this changes nothing.)
		for (const route::ButtonGlitch &glitch : driver.buttonGlitches) {
			if (glitch.actor != actor || glitch.tick + kButtonGlitchDelay != tick) continue;
			if (glitch.button.empty()) engine::Script("::PP.ReleaseAndPressAt(%.0f,%.0f,%.0f)", glitch.at.x, glitch.at.y, glitch.at.z);
			else engine::Script(glitch.endsPressed ? "::PP.ReleaseAndPress(%s)" : "::PP.PressAndRelease(%s)", Quote(glitch.button).c_str());
		}
	}
}

// Whether an object can be given a speed: the physics props (cubes, balls, boxes). A turret is a character
// with a physics shadow, which the game's catapult doesn't launch.
bool Launchable(const std::string &className) {
	return className.compare(0, 5, "prop_") == 0;
}

// One catapult per level, for giving objects a speed (the helper's Launch): made with the game's own
// ent_create, which spawns it (wherever the host looks; the helper moves it onto an object when needed).
void MakeCatapult() {
	if (g.catapultMade) return;
	g.catapultMade = true;
	engine::Execute("ent_create trigger_catapult");
	engine::Script("::PP.gravity <- %g;::PP.Catapult()", engine::CvarFloat("sv_gravity", 600));
}

// Whether the trainer moves a recorded object this tick. While the run plays itself, all of them. Once a
// person is in control, only what a driven player (the bot partner) carries or has let go of: the rest is the
// person's to move.
bool LooksAfterObject(size_t index, int tick) {
	const Route &driver = Driver();
	if (g.replaying || g.mode != Mode::Run) return true;
	int entity = driver.movers[index].entIndex;
	for (size_t i = 0; i < g.slots.size(); i++) {
		int actor = DriverActor(i);
		if (!Driven(g.slots[i]) || actor < 0) continue;
		if (driver.At(actor, tick).held == entity) g.objectLastDriven[index] = tick;
	}
	return g.objectLastDriven[index] >= 0;
}

// Keeps the run's objects (cubes, turrets, balls, ...) on the path they took in the run. Each is tied to its
// live counterpart when it first appears. Then:
// - What a player carries in the run is put on its recorded spot every tick, with the recorded turn and speed
//   (driven players hold nothing themselves): a laser cube points exactly where it pointed.
// - What moves on its own in the run (thrown, rolling, riding a funnel, settling on a button) is the game's to
//   move, here as there; it is put back on the recorded path only when it has strayed from it, and given the
//   run's speed again (a prop; the game can't be told a turret's speed). What a player lets go of is put on
//   its way at once: the throw is the game's from there. Buttons, ball sockets and catapults keep hold of an
//   object that is left alone.
// - What lies still in the run is looked at now and then, and put back if something has pushed it away.
// With `all` (the start of a replayed stretch) every object is tied and put in place.
void MoveObjects(bool all) {
	constexpr float kSlack = 4;            // how far an object moving on its own may be from the run's path
	constexpr float kTurnSlack = 5;        // and how many degrees off the way it lies, once it has stopped turning
	constexpr float kLaserCubeSlack = 1;   // the same for a reflection cube
	constexpr float kLaserCubeTurnSlack = 0.5f;
	constexpr int kLookAtResting = 30;     // ticks between looks at an object that lies still
	constexpr float kMovingSpeed = 30;     // slower than this, an object is left to settle on its own
	const Route &driver = Driver();
	int tick = DriverTick(g.cursor);
	if (g.objectLastDriven.size() != driver.movers.size()) g.objectLastDriven.assign(driver.movers.size(), -1);
	if (all) engine::Script("::PP.ForgetObjects()");
	// what the people in the game have in their hands is theirs. (Not what a player the trainer drives holds: the
	// game holds it in front of that player's own eyes, not where the run's player had it - a crouching carrier's
	// cube came onto Separation's button 36 units high. That stays on the run's path.)
	int inHands[2] = {-1, -1};
	size_t hands = 0;
	for (const Slot &slot : g.slots) {
		engine::PlayerState state;
		if (slot.entIndex && hands < 2 && engine::GetPlayer(slot.entIndex, state) && state.held > 0 && !Driven(slot))
			inHands[hands++] = state.held;
	}
	const float dt = driver.tickInterval;
	// what the run's fizzlers dissolved, dissolved here too while the run plays itself (see Route::dissolves)
	if (g.mode != Mode::Run || g.replaying)
		for (const route::Route::Dissolve &d : driver.dissolves)
			if (d.tick == tick) engine::Script("::PP.Dissolve(%s)", Quote(d.target).c_str());
	for (size_t i = 0; i < driver.movers.size(); i++) {
		const route::Mover &mover = driver.movers[i];
		Vec3 pos, ang;
		if (!mover.At(tick, pos, ang)) continue;
		int id = static_cast<int>(i);
		// (an object that turns up during the run - a dropper's cube - is taken only by a live one right there, for
		// its first two seconds: see PP.Claim)
		constexpr int kNewborn = 120;
		bool newborn = mover.keys.front().tick > driver.firstTick + 5 && tick - mover.keys.front().tick < kNewborn;
		float claimRadius = newborn ? 96.0f : 400.0f;
		if (all || tick == mover.keys.front().tick)
			engine::Script("::PP.Claim(%d,%s,%.1f,%.1f,%.1f,%.0f)", id, Quote(mover.className).c_str(), pos.x, pos.y, pos.z, claimRadius);
		if (!LooksAfterObject(i, tick)) continue;

		bool carried = false, carriedBefore = false;
		for (size_t a = 0; a < driver.actors.size(); a++) {
			if (driver.At(static_cast<int>(a), tick).held == mover.entIndex) carried = true;
			if (driver.At(static_cast<int>(a), tick - 1).held == mover.entIndex) carriedBefore = true;
		}
		bool released = carriedBefore && !carried;
		Vec3 before = pos, beforeAng = ang, after = pos, afterAng = ang;
		bool existed = mover.At(tick - 1, before, beforeAng);
		bool moving = !existed || Distance(before, pos) > 0.01f;
		bool turning = !existed || Distance(beforeAng, ang) > 0.5f;
		if (!all && !carried && !released && !moving && !turning && tick % kLookAtResting != 0) continue;

		// (its speed: where it is a tick later; none across a trip through a portal)
		constexpr float kPortalTrip = 64;
		Vec3 velocity{};
		if (mover.At(tick + 1, after, afterAng) && Distance(pos, after) < kPortalTrip) velocity = {(after.x - pos.x) / dt, (after.y - pos.y) / dt, (after.z - pos.z) / dt};
		// (how an object lies matters once it has stopped turning: a rolling ball may roll its own way. A
		// reflection cube is held to the run far more closely: a laser leaves it the way it faces, and Dual Lasers'
		// cube lying 4 degrees off sent the beam past its catcher.)
		// (and one that creeps along in the run - tipping over the edge of a button, sliding to a stop - is put on
		// every spot of that path: left to the game it creeps another way, and Turret Intro's cube, which lies
		// half on its button in the run, kept letting go of it.)
		bool creeping = !carried && (moving || turning) && Length(velocity) <= kMovingSpeed;
		float slack = (all || carried || released || creeping) ? 0 : mover.aimsLasers ? kLaserCubeSlack : kSlack;
		float turnSlack = turning ? 0 : mover.aimsLasers ? kLaserCubeTurnSlack : kTurnSlack;
		bool launch = !carried && Launchable(mover.className) && Length(velocity) > kMovingSpeed;
		engine::Script("::PP.Pose(%d,%s,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%.0f,%.0f,%.0f,%d,%d,%.1f,%.1f,%d,%.0f)", id, Quote(mover.className).c_str(), pos.x, pos.y,
		               pos.z, ang.x, ang.y, ang.z, velocity.x, velocity.y, velocity.z, inHands[0], inHands[1], slack, turnSlack, launch ? 1 : 0, claimRadius);
	}
}

// At the handover a person gets what their robot holds in the run at that moment: the object is right in front
// of them (the trainer has just put it there), and they pick it up as if they had pressed the use key.
void HandOverObjects() {
	if (g.replaying) return;
	for (size_t i = 0; i < g.slots.size(); i++) {
		const Slot &slot = g.slots[i];
		if (!slot.entIndex || !slot.human) continue;
		int held = g.route.At(static_cast<int>(i), g.handover).held;
		for (size_t m = 0; m < g.route.movers.size() && held >= 0; m++) {
			const route::Mover &mover = g.route.movers[m];
			Vec3 pos, ang;
			if (mover.entIndex != held || !mover.At(g.handover, pos, ang)) continue;
			engine::PlayerState live;
			if (engine::GetPlayer(slot.entIndex, live) && live.held > 0) continue;  // (already in their hands: the game's own catch)
			engine::Script("::PP.UseObject(%d,%d,%s,%.1f,%.1f,%.1f)", slot.entIndex, static_cast<int>(m), Quote(mover.className).c_str(), pos.x, pos.y, pos.z);
		}
	}
}

// A player the trainer is moving can't be hurt: a turret, a laser, goo or a crusher on the way would kill them
// and break the replay. Two guards, since the game has more than one way to hurt: a damage filter that lets
// nothing through, and its own god mode. `onlyDriven`: the people are on their own again; bots stay protected.
void ShieldPlayers(bool onlyDriven) {
	for (const Slot &slot : g.slots) {
		if (!slot.entIndex) continue;
		bool on = !onlyDriven || !slot.human || g.replaying;
		engine::Script("::PP.Shield(%d,%d)", slot.entIndex, on ? 1 : 0);
		engine::SetGodMode(slot.entIndex, on);
	}
}

// Portal shots of this tick. Shots by a player who is in control are theirs to make; everyone else's are placed.
//
// A demo shows a portal a couple of ticks after it really moved, compared with the players: the recording client
// sees its own player at once but the world's entities as the server last sent them. Played back at the demo's
// ticks, a portal shot lands late. Team Retrieval at 13.46 s: blue re-shoots the wall portal orange is just
// leaving, which pushes orange out at speed towards the exit; two ticks late, orange is already clear of the wall
// and gets nothing. Shots are placed kPortalLead ticks early, which lines the push up with the record's (it
// starts on the same tick as the record's).
constexpr int kPortalLead = 2;
int g_portalLead = kPortalLead;  // development: "prac_debug portallead N" overrides it
int g_followTicks = 0;           // development: "prac_debug follow <ticks>", see FollowTrace

void PlacePortals() {
	const Route &driver = Driver();
	int tick = DriverTick(g.cursor) + g_portalLead;
	// Every change up to `tick` not placed yet, each once: the first ones after SetPortals (which put the run's
	// portals as they were at the attempt's first tick) are already within the lead. Where the run jumped (a retry
	// back, a skip ahead), only this tick's.
	int from = g_portalsPlacedTo >= 0 && tick > g_portalsPlacedTo && tick - g_portalsPlacedTo <= g_portalLead + 1 ? g_portalsPlacedTo + 1 : tick;
	g_portalsPlacedTo = tick;
	for (const auto &change : driver.portals) {
		if (change.tick < from || change.tick > tick) continue;
		// (a person's shots from the handover on are theirs, also the ones the lead reaches before it)
		bool theirs = g.mode == Mode::Run || (g.mode == Mode::LeadIn && change.tick > DriverTick(g.handover));
		if (theirs && !g.replaying && change.actor >= 0 && change.actor < static_cast<int>(g.slots.size()) && g.slots[change.actor].human)
			continue;
		// (the run's previous change of this portal says whether it is open now: the attempt began with the
		// run's portals as they were, and has followed the run's changes since)
		bool open = false;
		for (const auto &before : driver.portals)
			if (before.tick < change.tick && before.portalEnt == change.portalEnt) open = before.active;
		if (g_followTicks > 0)
			engine::Print("[portal] tick %d actor %d ent %d portal2 %d active %d open %d pos %.1f %.1f %.1f ang %.1f %.1f %.1f\n", change.tick,
			              change.actor, change.portalEnt, change.portal2 ? 1 : 0, change.active ? 1 : 0, open ? 1 : 0, change.pos.x, change.pos.y,
			              change.pos.z, change.ang.x, change.ang.y, change.ang.z);
		ApplyPortalChange(driver, change, tick - g_portalLead, open);
	}
}

}  // namespace trainer
