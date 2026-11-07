// The Challenge Mode leaderboards at board.portal2.sr, as a source of demos: the board of a chamber (every
// run on it, with who, when and whether a demo is there) and the demos themselves, downloaded into a folder of
// the launcher's own, from where the library reads them like any other. Co-op runs are two entries on the
// board, one per robot, each with its own demo; they are shown as one.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Net;
using System.Text;
using System.Web.Script.Serialization;

namespace P2Practice {
	static class Boards {
		public const string Site = "https://board.portal2.sr";

		// the board's id of each chamber's map (a Portal 2 map not here isn't on the board)
		static readonly Dictionary<string, int> ids = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase) {
			{ "sp_a1_intro1", 62761 }, { "sp_a1_intro2", 62758 }, { "sp_a1_intro3", 47458 }, { "sp_a1_intro4", 47455 },
			{ "sp_a1_intro5", 47452 }, { "sp_a1_intro6", 47106 }, { "sp_a1_intro7", 62763 }, { "sp_a1_wakeup", 62759 },
			{ "sp_a2_intro", 47735 }, { "sp_a2_laser_intro", 62765 }, { "sp_a2_laser_stairs", 47736 },
			{ "sp_a2_dual_lasers", 47738 }, { "sp_a2_laser_over_goo", 47742 }, { "sp_a2_catapult_intro", 62767 },
			{ "sp_a2_trust_fling", 47744 }, { "sp_a2_pit_flings", 47465 }, { "sp_a2_fizzler_intro", 47746 },
			{ "sp_a2_sphere_peek", 47748 }, { "sp_a2_ricochet", 47751 }, { "sp_a2_bridge_intro", 47752 },
			{ "sp_a2_bridge_the_gap", 47755 }, { "sp_a2_turret_intro", 47756 }, { "sp_a2_laser_relays", 47759 },
			{ "sp_a2_turret_blocker", 47760 }, { "sp_a2_laser_vs_turret", 47763 }, { "sp_a2_pull_the_rug", 47764 },
			{ "sp_a2_column_blocker", 47766 }, { "sp_a2_laser_chaining", 47768 }, { "sp_a2_triple_laser", 47770 },
			{ "sp_a2_bts1", 47773 }, { "sp_a2_bts2", 47774 }, { "sp_a2_bts3", 47776 }, { "sp_a2_bts4", 47779 },
			{ "sp_a2_bts5", 47780 }, { "sp_a2_core", 62771 }, { "sp_a3_01", 47783 }, { "sp_a3_03", 47784 },
			{ "sp_a3_jump_intro", 47787 }, { "sp_a3_bomb_flings", 47468 }, { "sp_a3_crazy_box", 47469 },
			{ "sp_a3_transition01", 47472 }, { "sp_a3_speed_ramp", 47791 }, { "sp_a3_speed_flings", 47793 },
			{ "sp_a3_portal_intro", 47795 }, { "sp_a3_end", 47798 }, { "sp_a4_intro", 88350 }, { "sp_a4_tb_intro", 47800 },
			{ "sp_a4_tb_trust_drop", 47802 }, { "sp_a4_tb_wall_button", 47804 }, { "sp_a4_tb_polarity", 47806 },
			{ "sp_a4_tb_catch", 47808 }, { "sp_a4_stop_the_box", 47811 }, { "sp_a4_laser_catapult", 47813 },
			{ "sp_a4_laser_platform", 47815 }, { "sp_a4_speed_tb_catch", 47817 }, { "sp_a4_jump_polarity", 47819 },
			{ "sp_a4_finale1", 62776 }, { "sp_a4_finale2", 47821 }, { "sp_a4_finale3", 47824 }, { "sp_a4_finale4", 47456 },
			{ "mp_coop_doors", 47741 }, { "mp_coop_race_2", 47825 }, { "mp_coop_laser_2", 47828 },
			{ "mp_coop_rat_maze", 47829 }, { "mp_coop_laser_crusher", 45467 }, { "mp_coop_teambts", 46362 },
			{ "mp_coop_fling_3", 47831 }, { "mp_coop_infinifling_train", 47833 }, { "mp_coop_come_along", 47835 },
			{ "mp_coop_fling_1", 47837 }, { "mp_coop_catapult_1", 47840 }, { "mp_coop_multifling_1", 47841 },
			{ "mp_coop_fling_crushers", 47844 }, { "mp_coop_fan", 47845 }, { "mp_coop_wall_intro", 47848 },
			{ "mp_coop_wall_2", 47849 }, { "mp_coop_catapult_wall_intro", 47854 }, { "mp_coop_wall_block", 47856 },
			{ "mp_coop_catapult_2", 47858 }, { "mp_coop_turret_walls", 47861 }, { "mp_coop_turret_ball", 52642 },
			{ "mp_coop_wall_5", 52660 }, { "mp_coop_tbeam_redirect", 52662 }, { "mp_coop_tbeam_drill", 52663 },
			{ "mp_coop_tbeam_catch_grind_1", 52665 }, { "mp_coop_tbeam_laser_1", 52667 }, { "mp_coop_tbeam_polarity", 52671 },
			{ "mp_coop_tbeam_polarity2", 52687 }, { "mp_coop_tbeam_polarity3", 52689 }, { "mp_coop_tbeam_maze", 52691 },
			{ "mp_coop_tbeam_end", 52777 }, { "mp_coop_paint_come_along", 52694 }, { "mp_coop_paint_redirect", 52711 },
			{ "mp_coop_paint_bridge", 52714 }, { "mp_coop_paint_walljumps", 52715 }, { "mp_coop_paint_speed_fling", 52717 },
			{ "mp_coop_paint_red_racer", 52735 }, { "mp_coop_paint_speed_catch", 52738 },
			{ "mp_coop_paint_longjump_intro", 52740 }, { "mp_coop_separation_1", 49341 }, { "mp_coop_tripleaxis", 49343 },
			{ "mp_coop_catapult_catch", 49345 }, { "mp_coop_2paints_1bridge", 49347 }, { "mp_coop_paint_conversion", 49349 },
			{ "mp_coop_bridge_catch", 49351 }, { "mp_coop_laser_tbeam", 52757 }, { "mp_coop_paint_rat_maze", 52759 },
			{ "mp_coop_paint_crazy_box", 48287 },
		};

		// the board's chapters and courses, each by the map that opens it (in the order of Maps.Table)
		static readonly string[] chapters = {
			"sp_a1_intro1", "Chapter 1: The Courtesy Call", "sp_a2_laser_intro", "Chapter 2: The Cold Boot",
			"sp_a2_sphere_peek", "Chapter 3: The Return", "sp_a2_column_blocker", "Chapter 4: The Surprise",
			"sp_a2_bts3", "Chapter 5: The Escape", "sp_a3_00", "Chapter 6: The Fall", "sp_a3_speed_ramp", "Chapter 7: The Reunion",
			"sp_a4_intro", "Chapter 8: The Itch", "sp_a4_finale1", "Chapter 9: The Part Where He Kills You",
			"mp_coop_doors", "Course 1: Team Building", "mp_coop_fling_3", "Course 2: Mass and Velocity",
			"mp_coop_wall_intro", "Course 3: Hard-Light Surfaces", "mp_coop_tbeam_redirect", "Course 4: Excursion Funnels",
			"mp_coop_paint_come_along", "Course 5: Mobility Gels", "mp_coop_separation_1", "Course 6: Art Therapy",
		};

		public static bool OnBoard(string map) { return map != null && ids.ContainsKey(map); }

		public class Chapter {
			public string Name;
			public readonly List<KeyValuePair<string, string>> Chambers = new List<KeyValuePair<string, string>>(); // map file, shown name
		}

		// The chapters and courses with their chambers, in the game's order
		public static List<Chapter> Chapters() {
			var all = new List<Chapter>();
			foreach (var map in Maps.All()) {
				for (int i = 0; i < chapters.Length; i += 2)
					if (chapters[i] == map) all.Add(new Chapter { Name = chapters[i + 1] });
				if (all.Count > 0 && OnBoard(map)) all[all.Count - 1].Chambers.Add(new KeyValuePair<string, string>(map, Maps.Name(map)));
			}
			return all;
		}

		// Where the downloaded demos go (a folder the library reads)
		public static string Folder { get { return Path.Combine(App.ExeDir, "demos", "boards"); } }

		// One run on a chamber's board
		public class Entry {
			public int Rank;
			public int Score;          // hundredths of a second
			public string Player = "";
			public string SteamId = "";
			public DateTime Date;
			public int ChangelogId;
			public bool HasDemo;
			public string Video;       // a YouTube id, or null
			public Entry Partner;      // co-op: the other robot's entry

			public string TimeText { get { return FormatScore(Score); } }
			public string Players { get { return Partner == null ? Player : Player + "  +  " + Partner.Player; } }
			public string DateText { get { return Date == DateTime.MinValue ? "" : Date.ToLocalTime().ToString("d MMM yyyy", CultureInfo.CurrentCulture); } }
			public bool Complete { get { return HasDemo && (Partner == null || Partner.HasDemo); } }
			public string Key { get { return ChangelogId.ToString(CultureInfo.InvariantCulture); } }
		}

		public static string FormatScore(int score) {
			int cs = score % 100, s = score / 100 % 60, m = score / 6000;
			return (m > 0 ? m + ":" + s.ToString("00") : s.ToString()) + "." + cs.ToString("00");
		}

		static WebClient Client() {
			ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
			var c = new WebClient();
			c.Headers[HttpRequestHeader.UserAgent] = "P2Practice";
			return c;
		}

		// The chamber's board, best first (co-op pairs put together). Blocking: call it off the UI thread.
		public static List<Entry> Fetch(string map) {
			int id;
			if (!ids.TryGetValue(map, out id)) throw new InvalidOperationException(Maps.Name(map) + " isn't on the board.");
			string json;
			using (var c = Client()) json = Encoding.UTF8.GetString(c.DownloadData(Site + "/chamber/" + id + "/json"));
			var entries = new List<Entry>();
			var top = new JavaScriptSerializer { MaxJsonLength = int.MaxValue }.DeserializeObject(json) as Dictionary<string, object>;
			if (top == null) throw new InvalidDataException("The board's answer isn't what the launcher expects.");
			foreach (var kv in top) {
				var row = kv.Value as Dictionary<string, object>;
				if (row == null) continue;
				var score = Get(row, "scoreData") as Dictionary<string, object>;
				var user = Get(row, "userData") as Dictionary<string, object>;
				if (score == null) continue;
				var e = new Entry {
					SteamId = kv.Key,
					Player = user != null ? Convert.ToString(Get(user, "boardname") ?? "", CultureInfo.InvariantCulture) : "",
					Score = Int(Get(score, "score")),
					Rank = Int(Get(score, "scoreRank")),
					ChangelogId = Int(Get(score, "changelogId")),
					HasDemo = Int(Get(score, "hasDemo")) != 0,
					Video = Get(score, "autorender_id") as string ?? Get(score, "youtubeID") as string,
				};
				DateTime when;
				string date = Get(score, "date") as string;
				if (date != null && DateTime.TryParse(date, CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out when)) e.Date = when;
				if (e.ChangelogId > 0 && e.Score > 0) entries.Add(e);
			}
			entries.Sort((a, b) => a.Score != b.Score ? a.Score.CompareTo(b.Score) : a.Date.CompareTo(b.Date));
			// co-op: the two robots' entries of one run have the same time and date (seconds apart at most)
			if (map.StartsWith("mp_", StringComparison.OrdinalIgnoreCase)) {
				var paired = new List<Entry>();
				for (int i = 0; i < entries.Count; i++) {
					var e = entries[i];
					if (i + 1 < entries.Count && entries[i + 1].Score == e.Score && Math.Abs((entries[i + 1].Date - e.Date).TotalSeconds) < 90) {
						e.Partner = entries[i + 1];
						i++;
					}
					paired.Add(e);
				}
				entries = paired;
			}
			return entries;
		}

		static object Get(Dictionary<string, object> d, string key) { object v; return d.TryGetValue(key, out v) ? v : null; }

		static int Int(object v) {
			if (v == null) return 0;
			if (v is int) return (int)v;
			if (v is long) return (int)(long)v;
			if (v is bool) return (bool)v ? 1 : 0;
			int n;
			return int.TryParse(Convert.ToString(v, CultureInfo.InvariantCulture), NumberStyles.Integer, CultureInfo.InvariantCulture, out n) ? n : 0;
		}

		// The file a run's demo is kept in (the board's own naming: chamber, time, player, changelog id)
		public static string DemoPath(string map, Entry e) {
			return Path.Combine(Folder, Maps.Name(map).Replace(" ", "") + "_" + e.Score + "_" + e.SteamId + "_" + e.ChangelogId + ".dem");
		}

		public static bool Downloaded(string map, Entry e) {
			return File.Exists(DemoPath(map, e)) && (e.Partner == null || File.Exists(DemoPath(map, e.Partner)));
		}

		// Downloads a run's demo (both of a co-op pair). Blocking. Returns the files.
		public static List<string> Download(string map, Entry e) {
			var files = new List<string>();
			Directory.CreateDirectory(Folder);
			foreach (var one in e.Partner == null ? new[] { e } : new[] { e, e.Partner }) {
				string path = DemoPath(map, one);
				if (File.Exists(path)) { files.Add(path); continue; }
				string temp = path + ".part";
				using (var c = Client()) c.DownloadFile(Site + "/getDemo?id=" + one.ChangelogId, temp);
				var head = new byte[8];
				using (var f = File.OpenRead(temp)) f.Read(head, 0, 8);
				if (Encoding.ASCII.GetString(head, 0, 7) != "HL2DEMO") {
					File.Delete(temp);
					throw new InvalidDataException("The board sent no demo for " + one.Player + "'s run (it may not be public).");
				}
				File.Move(temp, path);
				files.Add(path);
			}
			return files;
		}
	}
}
