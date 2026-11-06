// Portal 2's maps in the order the game plays them, with the chamber names speedrunners use (the leaderboards'),
// for showing a demo's map by name and listing demos in game order.
using System;
using System.Collections.Generic;

namespace P2Practice {
	static class Maps {
		// file name, shown name
		static readonly string[] Table = {
			// single player
			"sp_a1_intro1", "Container Ride", "sp_a1_intro2", "Portal Carousel", "sp_a1_intro3", "Portal Gun",
			"sp_a1_intro4", "Smooth Jazz", "sp_a1_intro5", "Cube Momentum", "sp_a1_intro6", "Future Starter",
			"sp_a1_intro7", "Secret Panel", "sp_a1_wakeup", "Wakeup", "sp_a2_intro", "Incinerator",
			"sp_a2_laser_intro", "Laser Intro", "sp_a2_laser_stairs", "Laser Stairs", "sp_a2_dual_lasers", "Dual Lasers",
			"sp_a2_laser_over_goo", "Laser Over Goo", "sp_a2_catapult_intro", "Catapult Intro", "sp_a2_trust_fling", "Trust Fling",
			"sp_a2_pit_flings", "Pit Flings", "sp_a2_fizzler_intro", "Fizzler Intro",
			"sp_a2_sphere_peek", "Ceiling Catapult", "sp_a2_ricochet", "Ricochet", "sp_a2_bridge_intro", "Bridge Intro",
			"sp_a2_bridge_the_gap", "Bridge the Gap", "sp_a2_turret_intro", "Turret Intro", "sp_a2_laser_relays", "Laser Relays",
			"sp_a2_turret_blocker", "Turret Blocker", "sp_a2_laser_vs_turret", "Laser vs Turret", "sp_a2_pull_the_rug", "Pull the Rug",
			"sp_a2_column_blocker", "Column Blocker", "sp_a2_laser_chaining", "Laser Chaining", "sp_a2_triple_laser", "Triple Laser",
			"sp_a2_bts1", "Jail Break", "sp_a2_bts2", "Escape", "sp_a2_bts3", "Turret Factory", "sp_a2_bts4", "Turret Sabotage",
			"sp_a2_bts5", "Neurotoxin Sabotage", "sp_a2_bts6", "Tube Ride", "sp_a2_core", "Core",
			"sp_a3_00", "Long Fall", "sp_a3_01", "Underground", "sp_a3_03", "Cave Johnson", "sp_a3_jump_intro", "Repulsion Intro",
			"sp_a3_bomb_flings", "Bomb Flings", "sp_a3_crazy_box", "Crazy Box", "sp_a3_transition01", "PotatOS",
			"sp_a3_speed_ramp", "Propulsion Intro", "sp_a3_speed_flings", "Propulsion Flings", "sp_a3_portal_intro", "Conversion Intro",
			"sp_a3_end", "Three Gels",
			"sp_a4_intro", "Test", "sp_a4_tb_intro", "Funnel Intro", "sp_a4_tb_trust_drop", "Ceiling Button",
			"sp_a4_tb_wall_button", "Wall Button", "sp_a4_tb_polarity", "Polarity", "sp_a4_tb_catch", "Funnel Catch",
			"sp_a4_stop_the_box", "Stop the Box", "sp_a4_laser_catapult", "Laser Catapult", "sp_a4_laser_platform", "Laser Platform",
			"sp_a4_speed_tb_catch", "Propulsion Catch", "sp_a4_jump_polarity", "Repulsion Polarity",
			"sp_a4_finale1", "Finale 1", "sp_a4_finale2", "Finale 2", "sp_a4_finale3", "Finale 3", "sp_a4_finale4", "Finale 4",
			"sp_a5_credits", "Credits",
			// co-op
			"mp_coop_start", "Calibration", "mp_coop_lobby_2", "Hub", "mp_coop_lobby_3", "Hub",
			"mp_coop_doors", "Doors", "mp_coop_race_2", "Buttons", "mp_coop_laser_2", "Lasers", "mp_coop_rat_maze", "Rat Maze",
			"mp_coop_laser_crusher", "Laser Crusher", "mp_coop_teambts", "Behind the Scenes",
			"mp_coop_fling_3", "Flings", "mp_coop_infinifling_train", "Infinifling", "mp_coop_come_along", "Team Retrieval",
			"mp_coop_fling_1", "Vertical Flings", "mp_coop_catapult_1", "Catapults", "mp_coop_multifling_1", "Multifling",
			"mp_coop_fling_crushers", "Fling Crushers", "mp_coop_fan", "Industrial Fan",
			"mp_coop_wall_intro", "Cooperative Bridges", "mp_coop_wall_2", "Bridge Swap", "mp_coop_catapult_wall_intro", "Fling Block",
			"mp_coop_wall_block", "Catapult Block", "mp_coop_catapult_2", "Bridge Fling", "mp_coop_turret_walls", "Turret Walls",
			"mp_coop_turret_ball", "Turret Assassin", "mp_coop_wall_5", "Bridge Testing",
			"mp_coop_tbeam_redirect", "Cooperative Funnels", "mp_coop_tbeam_drill", "Funnel Drill",
			"mp_coop_tbeam_catch_grind_1", "Funnel Catch", "mp_coop_tbeam_laser_1", "Funnel Laser",
			"mp_coop_tbeam_polarity", "Cooperative Polarity", "mp_coop_tbeam_polarity2", "Funnel Hop",
			"mp_coop_tbeam_polarity3", "Advanced Polarity", "mp_coop_tbeam_maze", "Funnel Maze", "mp_coop_tbeam_end", "Turret Warehouse",
			"mp_coop_paint_come_along", "Repulsion Jumps", "mp_coop_paint_redirect", "Double Bounce",
			"mp_coop_paint_bridge", "Bridge Repulsion", "mp_coop_paint_walljumps", "Wall Repulsion",
			"mp_coop_paint_speed_fling", "Propulsion Crushers", "mp_coop_paint_red_racer", "Turret Ninja",
			"mp_coop_paint_speed_catch", "Propulsion Retrieval", "mp_coop_paint_longjump_intro", "Vault Entrance",
			"mp_coop_separation_1", "Separation", "mp_coop_tripleaxis", "Triple Axis", "mp_coop_catapult_catch", "Catapult Catch",
			"mp_coop_2paints_1bridge", "Bridge Gels", "mp_coop_paint_conversion", "Maintenance", "mp_coop_bridge_catch", "Bridge Catch",
			"mp_coop_laser_tbeam", "Double Lift", "mp_coop_paint_rat_maze", "Gel Maze", "mp_coop_paint_crazy_box", "Crazier Box",
		};

		static readonly Dictionary<string, int> index = Build();

		static Dictionary<string, int> Build() {
			var d = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
			for (int i = 0; i < Table.Length; i += 2) d[Table[i]] = i / 2;
			return d;
		}

		// the name to show: the chamber's, or the file name of a map that isn't the game's (workshop, mods)
		public static string Name(string map) {
			if (string.IsNullOrEmpty(map)) return "";
			int i;
			if (index.TryGetValue(map, out i)) return Table[i * 2 + 1];
			int slash = map.LastIndexOfAny(new[] { '/', '\\' }); // workshop/<id>/<name>
			return slash >= 0 && slash + 1 < map.Length ? map.Substring(slash + 1) : map;
		}

		// the map's place in play order; maps that aren't the game's come after, by name
		public static int Order(string map) {
			int i;
			return map != null && index.TryGetValue(map, out i) ? i : int.MaxValue;
		}
	}
}
