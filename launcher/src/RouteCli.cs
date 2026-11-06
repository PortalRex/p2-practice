// Talks to p2route.exe, the helper that reads demos. It answers with one JSON object on stdout:
//   p2route.exe scan <demo>                 quick: map, player, host or client, time
//   p2route.exe info <demo> [partner demo]  full: players and the run's parts
// Exit code 0 on success; otherwise 1 and {"ok":false,"error":"..."}.
using System;
using System.Collections;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Web.Script.Serialization;

namespace P2Practice {
	class ScanResult {
		public bool Ok;
		public string Error;      // why not, in the helper's words
		public bool Transient;    // the helper itself failed (missing, crashed, too slow): worth asking again next time
		public string Map = "";
		public bool Coop;
		public string Player = "";
		public bool Host;         // co-op: recorded by the host (ATLAS) rather than the client (P-body)
		public int Ticks;
		public double Time;       // seconds: the run's when HasTime, else the demo's length
		public string TimeText = "";
		public bool HasTime;      // the demo carries a SAR run time
	}

	class RoutePlayer {
		public string Team = "";  // "blue", "orange", or "" in single player
		public string Name = "";
		public bool HasInputs;
	}

	class RoutePart {
		public int Index;
		public double Start, End;
		public string Name = "";
	}

	class RouteInfo {
		public bool Ok;
		public string Error;
		public string Map = "";
		public bool Coop;
		public string TimeText = "";
		public readonly List<RoutePlayer> Players = new List<RoutePlayer>();
		public readonly List<RoutePart> Parts = new List<RoutePart>();
		public readonly List<string> Notes = new List<string>();
	}

	static class RouteCli {
		const int ScanTimeoutMs = 20000, InfoTimeoutMs = 120000;

		// p2route.exe next to the launcher; the P2ROUTE environment variable names another one (for testing)
		public static string ExePath {
			get {
				var over = Environment.GetEnvironmentVariable("P2ROUTE");
				return !string.IsNullOrEmpty(over) ? over : Path.Combine(App.ExeDir, "p2route.exe");
			}
		}

		public static bool Exists { get { return File.Exists(ExePath); } }

		public const string MissingMessage = "p2route.exe is missing next to P2Practice.exe, so demos can't be read";

		// changes whenever the helper is replaced, so answers cached from an older one are asked for again
		public static string Stamp {
			get {
				try {
					var f = new FileInfo(ExePath);
					return f.Exists ? f.Length + ":" + f.LastWriteTimeUtc.Ticks : "none";
				} catch (IOException) { return "none"; } catch (UnauthorizedAccessException) { return "none"; }
			}
		}

		public static ScanResult Scan(string demo) {
			var r = new ScanResult();
			var j = Run("scan " + Quote(demo), ScanTimeoutMs, out r.Error, out r.Transient);
			if (j == null) return r;
			r.Ok = true;
			r.Map = Str(j, "map");
			r.Coop = Bool(j, "coop");
			r.Player = Str(j, "player");
			r.Host = Bool(j, "host");
			r.Ticks = (int)Num(j, "ticks");
			r.Time = Num(j, "time");
			r.HasTime = Bool(j, "hasTime");
			r.TimeText = Str(j, "timeText");
			if (r.TimeText.Length == 0) r.TimeText = FormatTime(r.Time);
			if (r.Map.Length == 0) { r.Ok = false; r.Error = "the demo names no map"; }
			return r;
		}

		public static RouteInfo Info(string demo, string partner) {
			var r = new RouteInfo();
			bool transient;
			var j = Run("info " + Quote(demo) + (partner != null ? " " + Quote(partner) : ""), InfoTimeoutMs, out r.Error, out transient);
			if (j == null) return r;
			r.Ok = true;
			r.Map = Str(j, "map");
			r.Coop = Bool(j, "coop");
			r.TimeText = Str(j, "timeText");
			foreach (var p in List(j, "players")) {
				var d = p as Dictionary<string, object>;
				if (d != null) r.Players.Add(new RoutePlayer { Team = Str(d, "team").ToLowerInvariant(), Name = Str(d, "name"), HasInputs = Bool(d, "hasInputs") });
			}
			foreach (var p in List(j, "parts")) {
				var d = p as Dictionary<string, object>;
				if (d != null) r.Parts.Add(new RoutePart { Index = (int)Num(d, "index"), Start = Num(d, "start"), End = Num(d, "end"), Name = Str(d, "name") });
			}
			// parts are started by number; one without a usable number can't be asked for
			r.Parts.RemoveAll(p => p.Index <= 0);
			foreach (var n in List(j, "notes")) if (n != null) r.Notes.Add(Convert.ToString(n, CultureInfo.InvariantCulture));
			return r;
		}

		// Runs the helper and returns its answer, or null with the reason.
		static Dictionary<string, object> Run(string args, int timeoutMs, out string error, out bool transient) {
			error = null;
			transient = true;
			string exe = ExePath;
			if (!File.Exists(exe)) { error = MissingMessage; return null; }
			string text;
			int exit;
			try {
				var psi = new ProcessStartInfo(exe, args) {
					UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, StandardOutputEncoding = Encoding.UTF8,
					WorkingDirectory = Path.GetDirectoryName(Path.GetFullPath(exe)),
				};
				using (var p = Process.Start(psi)) {
					// a big library is read in the background: don't slow a running game down for it
					try { p.PriorityClass = ProcessPriorityClass.BelowNormal; } catch (InvalidOperationException) { } catch (Win32Exception) { }
					var read = p.StandardOutput.ReadToEndAsync();
					if (!p.WaitForExit(timeoutMs)) {
						try { p.Kill(); } catch (InvalidOperationException) { } catch (Win32Exception) { }
						error = "p2route.exe took too long and was stopped";
						return null;
					}
					p.WaitForExit(); // lets the output finish arriving
					text = read.Result;
					exit = p.ExitCode;
				}
			} catch (Win32Exception ex) {
				error = "p2route.exe could not be started: " + ex.Message;
				return null;
			} catch (Exception ex) {
				if (!(ex is IOException || ex is InvalidOperationException || ex is AggregateException)) throw;
				error = "p2route.exe could not be run: " + ex.Message;
				return null;
			}

			// the answer is the one JSON object; anything printed around it (a byte order mark, a warning) is not
			int open = text.IndexOf('{'), close = text.LastIndexOf('}');
			Dictionary<string, object> j = null;
			if (open >= 0 && close > open) {
				try { j = new JavaScriptSerializer { MaxJsonLength = int.MaxValue }.DeserializeObject(text.Substring(open, close - open + 1)) as Dictionary<string, object>; }
				catch (ArgumentException) { } catch (InvalidOperationException) { }
			}
			if (j == null) {
				error = "p2route.exe gave no answer (exit code " + exit + ")"; // crashed, or isn't the helper at all
				return null;
			}
			transient = false;
			if (exit != 0 || !Bool(j, "ok")) {
				error = Str(j, "error");
				if (error.Length == 0) error = "p2route.exe could not read the demo (exit code " + exit + ")";
				return null;
			}
			return j;
		}

		// One argument as the C runtime's command-line rules read it back: quoted, with the backslashes before a
		// quote (or the end) doubled.
		public static string Quote(string arg) {
			var sb = new StringBuilder("\"");
			int slashes = 0;
			foreach (char c in arg) {
				if (c == '\\') { slashes++; continue; }
				if (c == '"') { sb.Append('\\', slashes * 2 + 1); sb.Append('"'); }
				else { sb.Append('\\', slashes); sb.Append(c); }
				slashes = 0;
			}
			sb.Append('\\', slashes * 2);
			return sb.Append('"').ToString();
		}

		// seconds as the game's timers show them: 6.46, 1:02.30 (hundredths, cut off rather than rounded)
		public static string FormatTime(double seconds) {
			if (double.IsNaN(seconds) || double.IsInfinity(seconds) || seconds < 0) seconds = 0;
			long cs = (long)Math.Floor(seconds * 100 + 1e-6);
			long min = cs / 6000;
			double rest = (cs % 6000) / 100.0;
			return min > 0 ? min.ToString(CultureInfo.InvariantCulture) + ":" + rest.ToString("00.00", CultureInfo.InvariantCulture)
			               : rest.ToString("0.00", CultureInfo.InvariantCulture);
		}

		static string Str(Dictionary<string, object> j, string key) {
			object v;
			return j.TryGetValue(key, out v) && v != null ? Convert.ToString(v, CultureInfo.InvariantCulture) : "";
		}

		static bool Bool(Dictionary<string, object> j, string key) {
			object v;
			if (!j.TryGetValue(key, out v) || v == null) return false;
			if (v is bool) return (bool)v;
			var s = Convert.ToString(v, CultureInfo.InvariantCulture);
			return s == "1" || s.Equals("true", StringComparison.OrdinalIgnoreCase);
		}

		static double Num(Dictionary<string, object> j, string key) {
			object v;
			if (!j.TryGetValue(key, out v) || v == null) return 0;
			try {
				double d = Convert.ToDouble(v, CultureInfo.InvariantCulture);
				return double.IsNaN(d) || double.IsInfinity(d) ? 0 : d;
			} catch (FormatException) { return 0; } catch (InvalidCastException) { return 0; } catch (OverflowException) { return 0; }
		}

		static IEnumerable List(Dictionary<string, object> j, string key) {
			object v;
			if (j.TryGetValue(key, out v) && v is IEnumerable && !(v is string)) return (IEnumerable)v;
			return new object[0];
		}
	}
}
