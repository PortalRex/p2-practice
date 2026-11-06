using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace P2Practice {
	// How the player wants to practise. Each one is a plugin setting (prac_set / prac_role / prac_auto).
	class Options {
		public bool Watch;              // ride along with the run instead of practising it
		public bool Orange;             // co-op alone: play P-body (otherwise ATLAS)
		public bool RealPartner;        // co-op: a real partner plays the other robot (otherwise a bot does)
		public bool Ghost = true;       // show the recorded players
		public bool Clock = true;       // on-screen clock ("hud")
		public bool Keys = true;        // the keys the record holds
		public bool Line = true;        // the route drawn in the level ("path")
		public double LeadIn = 1.5;     // seconds the run plays itself before the player takes over
		public double Speed = 1.0;      // game speed while practising
		public bool Loop;               // start again by itself after the finish
		public string Extra = "";       // the player's own launch options, appended as typed

		public const double MinLeadIn = 0, MaxLeadIn = 10, MinSpeed = 0.05, MaxSpeed = 4; // the plugin's limits

		// one option by its ini name; false when the name or value means nothing
		public bool Set(string key, string value) {
			bool on = value == "1";
			double d;
			bool num = double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out d);
			switch (key) {
				case "mode": Watch = value == "watch"; return true;
				case "role": Orange = value == "orange"; return true;
				case "partner": RealPartner = value == "real"; return true;
				case "ghost": Ghost = on; return true;
				case "clock": Clock = on; return true;
				case "keys": Keys = on; return true;
				case "line": Line = on; return true;
				case "loop": Loop = on; return true;
				case "leadin": if (num) LeadIn = Math.Max(MinLeadIn, Math.Min(MaxLeadIn, d)); return num;
				case "speed": if (num) Speed = Math.Max(MinSpeed, Math.Min(MaxSpeed, d)); return num;
				case "extra": Extra = value; return true;
			}
			return false;
		}

		public IEnumerable<string> Lines() {
			yield return "mode=" + (Watch ? "watch" : "practise");
			yield return "role=" + (Orange ? "orange" : "blue");
			yield return "partner=" + (RealPartner ? "real" : "bot");
			yield return "ghost=" + (Ghost ? 1 : 0);
			yield return "clock=" + (Clock ? 1 : 0);
			yield return "keys=" + (Keys ? 1 : 0);
			yield return "line=" + (Line ? 1 : 0);
			yield return "leadin=" + Num(LeadIn);
			yield return "speed=" + Num(Speed);
			yield return "loop=" + (Loop ? 1 : 0);
			yield return "extra=" + Extra;
		}

		// numbers as the game's console reads them: a point, no thousands separators, no trailing zeros
		public static string Num(double v) { return v.ToString("0.###", CultureInfo.InvariantCulture); }
	}

	// Everything remembered between runs, in P2Practice.ini next to the exe.
	class Settings {
		public string GameDir;                                   // the Portal 2 folder: chosen by hand, or found through Steam
		public bool GameChosen;                                  // chosen by hand (kept even if Steam says otherwise)
		public readonly List<string> Folders = new List<string>(); // demo folders, searched recursively
		public bool FoldersSaved;                                // the list in the ini is the player's own (no defaults added)
		public readonly Options Options = new Options();
		// window, in 100%-scale units so it fits again on a monitor with another scale
		public int Width, Height, Left = int.MinValue, Top = int.MinValue;
		public bool Maximized;
		public int SortColumn, SortDescending;

		static string FilePath { get { return Path.Combine(App.Home, "P2Practice.ini"); } }

		public static Settings Load() {
			var s = new Settings();
			try {
				foreach (var raw in File.ReadAllLines(FilePath, Encoding.UTF8)) {
					var line = raw.Trim();
					int eq = line.IndexOf('=');
					if (eq <= 0 || line[0] == ';' || line[0] == '[') continue;
					string k = line.Substring(0, eq).Trim().ToLowerInvariant(), v = line.Substring(eq + 1).Trim();
					int n;
					bool num = int.TryParse(v, NumberStyles.Integer, CultureInfo.InvariantCulture, out n);
					switch (k) {
						case "game": s.GameDir = v; s.GameChosen = v.Length > 0; break;
						case "folders": s.FoldersSaved = true; break;
						case "folder": if (v.Length > 0 && !s.HasFolder(v)) s.Folders.Add(v); break;
						case "width": if (num) s.Width = n; break;
						case "height": if (num) s.Height = n; break;
						case "left": if (num) s.Left = n; break;
						case "top": if (num) s.Top = n; break;
						case "maximized": s.Maximized = v == "1"; break;
						case "sort": if (num) s.SortColumn = n; break;
						case "sortdescending": s.SortDescending = v == "1" ? 1 : 0; break;
						default: s.Options.Set(k, v); break;
					}
				}
			} catch (IOException) { } catch (UnauthorizedAccessException) { }

			if (App.GameOverride != null) { s.GameDir = App.GameOverride; s.GameChosen = false; }
			else if (!GameLocator.IsGameDir(s.GameDir)) { s.GameDir = GameLocator.Find(); s.GameChosen = false; }
			if (!s.FoldersSaved) s.AddDefaultFolders();
			foreach (var set in App.Sets) s.Options.Set(set.Key.ToLowerInvariant(), set.Value);
			return s;
		}

		// the game's own demos folder if there is one, and a "demos" folder next to the exe (to drop demos into)
		public void AddDefaultFolders() {
			if (GameLocator.IsGameDir(GameDir)) {
				var d = Path.Combine(GameDir, "portal2", "demos");
				if (Directory.Exists(d) && !HasFolder(d)) Folders.Add(d);
			}
			var own = Path.Combine(App.ExeDir, "demos");
			if (!HasFolder(own)) Folders.Add(own);
		}

		public bool HasFolder(string dir) {
			foreach (var f in Folders) if (SamePath(f, dir)) return true;
			return false;
		}

		public static bool SamePath(string a, string b) {
			return string.Equals(a.TrimEnd('\\', '/'), b.TrimEnd('\\', '/'), StringComparison.OrdinalIgnoreCase);
		}

		public void Save() {
			if (App.Headless && !App.SaveSettings) return; // a screenshot run leaves the player's settings alone
			var lines = new List<string>();
			lines.Add("; P2 Practice launcher settings");
			lines.Add("[launcher]");
			lines.Add("game=" + (GameChosen ? GameDir : "")); // empty: look it up through Steam each time
			lines.Add("folders=" + Folders.Count);
			foreach (var f in Folders) lines.Add("folder=" + f);
			lines.Add("width=" + Width);
			lines.Add("height=" + Height);
			if (Left != int.MinValue) { lines.Add("left=" + Left); lines.Add("top=" + Top); }
			lines.Add("maximized=" + (Maximized ? 1 : 0));
			lines.Add("sort=" + SortColumn);
			lines.Add("sortdescending=" + SortDescending);
			lines.Add("");
			lines.Add("[options]");
			lines.AddRange(Options.Lines());
			try {
				Directory.CreateDirectory(App.Home);
				File.WriteAllLines(FilePath, lines, new UTF8Encoding(false));
			} catch (IOException) { } catch (UnauthorizedAccessException) { }
		}
	}
}
