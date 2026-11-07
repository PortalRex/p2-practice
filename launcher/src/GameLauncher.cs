// Starts Portal 2 with the plugin loaded and the practice session set up - or, when the game is already
// running, leaves the same commands in a cfg for the player to exec.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;
using Microsoft.Win32;

namespace P2Practice {
	// Finds the Portal 2 install through Steam's library folders.
	static class GameLocator {
		public static bool IsGameDir(string dir) {
			try { return !string.IsNullOrEmpty(dir) && File.Exists(Path.Combine(dir, "portal2.exe")); }
			catch (ArgumentException) { return false; }
		}

		static string SteamValue(string name) {
			try {
				var v = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", name, null) as string;
				return string.IsNullOrEmpty(v) ? null : v.Replace('/', '\\');
			} catch (Exception ex) {
				if (ex is System.Security.SecurityException || ex is IOException || ex is UnauthorizedAccessException) return null;
				throw;
			}
		}

		public static string Find() {
			var libraries = new List<string>();
			string steam = SteamValue("SteamPath");
			if (steam != null) {
				libraries.Add(steam);
				try {
					var vdf = File.ReadAllText(Path.Combine(steam, "steamapps", "libraryfolders.vdf"));
					foreach (Match m in Regex.Matches(vdf, "\"path\"\\s+\"([^\"]+)\""))
						libraries.Add(m.Groups[1].Value.Replace(@"\\", @"\"));
				} catch (IOException) { } catch (UnauthorizedAccessException) { }
			}
			libraries.Add(@"C:\Program Files (x86)\Steam"); // Steam's usual place, should the registry say nothing
			foreach (var lib in libraries) {
				try {
					var dir = Path.Combine(lib, "steamapps", "common", "Portal 2");
					if (IsGameDir(dir)) return ActualCase(dir);
				} catch (ArgumentException) { }
			}
			return null;
		}

		// steam.exe, or null when Steam isn't installed where the registry says
		public static string SteamExe() {
			string exe = SteamValue("SteamExe");
			if (exe == null) {
				string dir = SteamValue("SteamPath");
				if (dir != null) exe = Path.Combine(dir, "steam.exe");
			}
			try { return exe != null && File.Exists(exe) ? ActualCase(exe) : null; }
			catch (ArgumentException) { return null; }
		}

		// Steam stores its path in lower case; show the folder as Explorer does.
		static string ActualCase(string path) {
			try {
				var d = new DirectoryInfo(path);
				var parts = new List<string>();
				while (d.Parent != null) {
					var real = d.Parent.GetFileSystemInfos(d.Name).FirstOrDefault();
					parts.Insert(0, real != null ? real.Name : d.Name);
					d = d.Parent;
				}
				parts.Insert(0, d.Name.ToUpperInvariant());
				return Path.Combine(parts.ToArray());
			} catch (IOException) { return path; } catch (UnauthorizedAccessException) { return path; }
		}
	}

	class LaunchRequest {
		public string GameDir;
		public string Demo, Partner;   // Partner: the other player's demo of the same co-op run, or null
		public string Map, MapName;
		public bool Coop;
		public int Part = 1;           // the part to start from
		public Options Options;
	}

	class LaunchResult {
		public bool Ok;
		public string Message;         // for the status line
	}

	static class GameLauncher {
		public const string PluginFile = "p2practice.dll";
		public const string LaunchCfg = "p2practice_launch";
		const string SteamAppId = "620";
		const int MaxConsoleCommand = 500; // the game's console cuts a command off a little after this

		// for tests: say whether the game / Steam counts as running, instead of looking (--game-running, --steam-running)
		public static bool? GameRunningOverride, SteamRunningOverride;

		enum GameState { NotRunning, Running, AnotherCopy }

		public static LaunchResult Launch(LaunchRequest r) {
			if (!GameLocator.IsGameDir(r.GameDir)) return Fail("Portal 2 wasn't found. Choose its folder under Game > Choose Portal 2 folder.");
			if (!Regex.IsMatch(r.Map ?? "", @"^[A-Za-z0-9_][A-Za-z0-9_\-./]*$")) return Fail("This demo's map name (" + r.Map + ") can't be given to the game.");
			if (!File.Exists(r.Demo)) return Fail("The demo is no longer there: " + r.Demo);
			if (r.Partner != null && !File.Exists(r.Partner)) return Fail("The partner's demo is no longer there: " + r.Partner);

			// a dry run changes nothing outside the launcher's own folder (unless a test asks it to, in a made-up game folder)
			bool writes = !App.DryRun || App.ApplyFiles;
			var notes = new List<string>();

			string other;
			var state = FindGame(r.GameDir, out other);
			if (state == GameState.AnotherCopy) return Fail("Another Portal 2 is running (" + other + "). Close it first: only one can run at a time.");
			bool running = state == GameState.Running;

			string demo, partner, error;
			if (!StageDemos(r, writes, notes, out demo, out partner, out error)) return Fail(error);
			if (!InstallPlugin(r.GameDir, running, writes, notes, out error)) return Fail(error);

			var commands = Commands(r, demo, partner);
			string extra = Regex.Replace(r.Options.Extra ?? "", @"\s+", " ").Trim();
			string what = r.MapName + ", " + (r.Options.Watch ? "watching from part " : "part ") + Math.Max(1, r.Part);
			string tail = notes.Count > 0 ? "  " + string.Join(" ", notes) : "";

			// The session is a cfg the game executes: loading the plugin, the settings, the run, the map. One "exec"
			// starts it, on the command line of a new game or in the console of one that is already up. (The plugin
			// can't be loaded with +plugin_load here: the engine honours only one on its command line, and Steam adds
			// the player's own launch options, which may already hold one for SAR.)
			string cfg = Path.Combine(r.GameDir, "portal2", "cfg", LaunchCfg + ".cfg");
			var lines = new List<string>(commands);
			if (running) lines.AddRange(ExtraCommands(extra)); // a new game takes these from its command line instead
			if (App.DryRun) {
				App.Log("launch: " + what + (running ? " - Portal 2 counts as running, so nothing is started:" : ""));
				App.Log((writes ? "wrote " : "would write ") + cfg + ":");
				foreach (var l in lines) App.Log("  " + l);
			}
			if (writes) {
				try {
					Directory.CreateDirectory(Path.GetDirectoryName(cfg));
					File.WriteAllLines(cfg, lines, Encoding.Default); // the game reads cfgs in the system code page
				} catch (Exception ex) {
					if (!(ex is IOException || ex is UnauthorizedAccessException)) throw;
					return Fail("Couldn't write " + cfg + ": " + ex.Message);
				}
			}

			if (running) {
				string paste = "exec " + LaunchCfg;
				if (App.DryRun) {
					App.Log("would copy to the clipboard: " + paste);
					foreach (var n in notes) App.Log("note: " + n);
				}
				if (App.DryRun) return Done("Dry run (Portal 2 counts as running): " + (writes ? "wrote " : "would write ") + cfg + " and would copy \"" + paste + "\" to the clipboard. See " + App.LogPath + tail);
				bool copied = true;
				try { Clipboard.SetText(paste); } catch (ExternalException) { copied = false; } // another program is holding the clipboard
				return Done("Portal 2 is already running. Open its console and " + (copied ? "paste:  " + paste + "  (it's on the clipboard)." : "type:  " + paste) + tail);
			}

			// start the game: through Steam when it's running (as the library does), else directly
			string args = "-novid +exec " + LaunchCfg + (extra.Length > 0 ? " " + extra : "");
			string steam = SteamRunning() ? GameLocator.SteamExe() : null;
			string exe = steam ?? Path.Combine(r.GameDir, "portal2.exe");
			args = (steam != null ? "-applaunch " + SteamAppId + " " : "-steam ") + args;
			string commandLine = "\"" + exe + "\" " + args;
			if (App.DryRun) {
				App.Log("launch: " + what);
				App.Log("command line: " + commandLine);
				App.Log("working directory: " + r.GameDir);
				foreach (var n in notes) App.Log("note: " + n);
				return Done("Dry run, nothing started: " + commandLine + tail);
			}
			try {
				using (Process.Start(new ProcessStartInfo(exe, args) { UseShellExecute = false, WorkingDirectory = r.GameDir })) { }
			} catch (Exception ex) {
				if (!(ex is Win32Exception || ex is IOException || ex is InvalidOperationException)) throw;
				return Fail("Portal 2 couldn't be started: " + ex.Message);
			}
			return Done("Starting Portal 2" + (steam != null ? " through Steam" : "") + ": " + what + "." + tail);
		}

		static LaunchResult Fail(string message) {
			if (App.DryRun) App.Log("launch refused: " + message);
			return new LaunchResult { Message = message };
		}
		static LaunchResult Done(string message) { return new LaunchResult { Ok = true, Message = message }; }

		// The console commands that set the session up, in the order the plugin wants them (without the leading +).
		public static List<string> Commands(LaunchRequest r, string demo, string partner) {
			var o = r.Options;
			bool bot = r.Coop && !o.RealPartner; // alone in co-op: split screen, a bot plays the other robot
			var c = new List<string>();
			c.Add("plugin_load p2practice");
			c.Add("prac_set leadin " + Options.Num(o.LeadIn));
			c.Add("prac_set ghost " + (o.Ghost ? 1 : 0));
			c.Add("prac_set ghostcolor " + (o.GhostColor == "" ? "team" : o.GhostColor));
			c.Add("prac_set ghostopacity " + Options.Num(o.GhostOpacity));
			c.Add("prac_set hud " + (o.Clock ? 1 : 0));
			c.Add("prac_set keys " + (o.Keys ? 1 : 0));
			c.Add("prac_set path " + (o.Line ? 1 : 0));
			c.Add("prac_set speed " + Options.Num(o.Speed));
			c.Add("prac_set loop " + (o.Loop ? 1 : 0));
			c.Add("prac_set partner " + (r.Coop && o.RealPartner ? 0 : 1));
			// with a real partner the host is ATLAS and the client P-body, as always: nothing to choose
			c.Add("prac_role " + (bot ? (o.Orange ? "orange" : "blue") : "auto"));
			c.Add("prac_load \"" + demo + "\"" + (partner != null ? " \"" + partner + "\"" : ""));
			c.Add("prac_auto " + (o.Watch ? "watch " : "") + Math.Max(1, r.Part));
			// with a real partner the player hosts the map as usual: the route is loaded and armed, and starts there
			if (!r.Coop) c.Add("map " + r.Map);
			else if (bot) c.Add("ss_map " + r.Map);
			return c;
		}

		// The "+command arguments" among the player's own launch options, for the cfg ("-switches" only mean
		// something when the game starts).
		static List<string> ExtraCommands(string extra) {
			var result = new List<string>();
			StringBuilder current = null;
			foreach (Match m in Regex.Matches(extra, "(?:\"[^\"]*\"?|\\S)+")) {
				string t = m.Value;
				bool isSwitch = t.Length > 1 && t[0] == '-' && !char.IsDigit(t[1]) && t[1] != '.';
				if ((t[0] == '+' && t.Length > 1) || isSwitch) {
					if (current != null) result.Add(current.ToString());
					current = isSwitch ? null : new StringBuilder(t.Substring(1));
				} else if (current != null) current.Append(' ').Append(t);
			}
			if (current != null) result.Add(current.ToString());
			return result;
		}

		// ---- demo paths the game can't take ----

		// The game reads its command line and cfgs in the system's code page, ends a console command at ';' and
		// can't have a '"' inside a quoted argument.
		static bool SafeForGame(string path) {
			foreach (char c in path) if (c < 32 || c == '"' || c == ';') return false;
			try {
				var ansi = Encoding.Default;
				return ansi.GetString(ansi.GetBytes(path)) == path;
			} catch (ArgumentException) { return false; }
		}

		// Uses the demos where they are when the game can read their paths; otherwise copies them aside under a
		// plain name and uses the copies.
		static bool StageDemos(LaunchRequest r, bool writes, List<string> notes, out string demo, out string partner, out string error) {
			demo = r.Demo;
			partner = r.Partner;
			error = null;
			bool tooLong = ("prac_load \"" + demo + "\"" + (partner != null ? " \"" + partner + "\"" : "")).Length > MaxConsoleCommand;
			bool stageDemo = tooLong || !SafeForGame(demo), stagePartner = partner != null && (tooLong || !SafeForGame(partner));

			// next to the launcher if the game can read that path, else in the game's own demos folder
			string own = Path.Combine(App.Home, "staged"), inGame = Path.Combine(r.GameDir, "portal2", "demos", "p2practice");
			string dir = SafeForGame(own) && own.Length <= 150 ? own : inGame;
			if (!stageDemo && !stagePartner) {
				if (writes) { RemoveCopies(own, null, null); RemoveCopies(inGame, null, null); } // an earlier launch's
				return true;
			}
			if (!SafeForGame(dir)) {
				error = "This demo's path has characters Portal 2 can't read (like ; or letters outside this PC's language). Move the demo to a plainly named folder.";
				return false;
			}
			try {
				if (stageDemo) demo = Stage(r.Demo, dir, writes, notes);
				if (stagePartner) partner = Stage(r.Partner, dir, writes, notes);
				if (writes) RemoveCopies(dir, demo, partner); // only the copies of this launch stay
			} catch (Exception ex) {
				if (!(ex is IOException || ex is UnauthorizedAccessException)) throw;
				error = "This demo's path has characters Portal 2 can't read, and a copy couldn't be made in " + dir + ": " + ex.Message;
				return false;
			}
			return true;
		}

		// deletes the demo copies in a staging folder (files named the way Stage names them), except the two named
		static void RemoveCopies(string dir, string keep, string keepToo) {
			try {
				if (!Directory.Exists(dir)) return;
				foreach (var old in Directory.GetFiles(dir, "*.dem")) {
					if (!Regex.IsMatch(Path.GetFileName(old), @"-[0-9a-f]{8}\.dem$", RegexOptions.IgnoreCase)) continue; // not one of ours
					if ((keep != null && Settings.SamePath(old, keep)) || (keepToo != null && Settings.SamePath(old, keepToo))) continue;
					try { File.Delete(old); } catch (IOException) { } catch (UnauthorizedAccessException) { }
				}
			} catch (IOException) { } catch (UnauthorizedAccessException) { }
		}

		static string Stage(string source, string dir, bool writes, List<string> notes) {
			// a plain name that stays the same for the same demo: its letters and digits, and a number made from its path
			string plain = Regex.Replace(Path.GetFileNameWithoutExtension(source), "[^A-Za-z0-9_.-]", "_");
			if (plain.Length > 60) plain = plain.Substring(0, 60);
			if (plain.Trim('_', '.', '-').Length == 0) plain = "demo";
			uint hash = 2166136261;
			foreach (char c in source.ToLowerInvariant()) hash = (hash ^ c) * 16777619;
			string target = Path.Combine(dir, plain + "-" + hash.ToString("x8") + ".dem");
			if (!writes) {
				notes.Add("Would copy " + Path.GetFileName(source) + " to " + target + " (the game can't read its path).");
				return target;
			}
			Directory.CreateDirectory(dir);
			var from = new FileInfo(source);
			var to = new FileInfo(target);
			if (!to.Exists || to.Length != from.Length || to.LastWriteTimeUtc != from.LastWriteTimeUtc) File.Copy(source, target, true);
			notes.Add("Using a copy of " + Path.GetFileName(source) + " (the game can't read its path).");
			return target;
		}

		// ---- the plugin ----

		// Puts the p2practice.dll that ships next to the launcher into the game folder when the one there is
		// missing or another build. False (with the reason) when there is no plugin to launch with.
		static bool InstallPlugin(string gameDir, bool running, bool writes, List<string> notes, out string error) {
			error = null;
			string shipped = Path.Combine(App.ExeDir, PluginFile), installed = Path.Combine(gameDir, PluginFile);
			bool have = File.Exists(installed);
			if (!File.Exists(shipped)) {
				if (have) { notes.Add(PluginFile + " isn't next to P2Practice.exe; using the one already in the game folder."); return true; }
				error = PluginFile + " is missing next to P2Practice.exe, so the plugin can't be put in the game folder.";
				if (!App.DryRun) return false;
				notes.Add(error); // a dry run still shows what it would start
				error = null;
				return true;
			}
			try {
				if (have && SameContent(shipped, installed)) return true;
				if (!writes) {
					notes.Add("Would " + (have ? "update" : "install") + " the plugin (copy " + shipped + " to " + installed + ").");
					return true;
				}
				File.Copy(shipped, installed, true);
				notes.Add(have ? "Plugin updated in the game folder." : "Plugin installed in the game folder.");
				return true;
			} catch (Exception ex) {
				if (!(ex is IOException || ex is UnauthorizedAccessException)) throw;
				if (have) {
					// the game keeps a loaded plugin's file open
					notes.Add(running ? "The plugin in the game folder couldn't be updated because Portal 2 is running; the game keeps using the one it has. Close it to update."
					                  : "The plugin in the game folder couldn't be updated (" + ex.Message + "); using the one there.");
					return true;
				}
				error = "The plugin couldn't be copied to " + installed + ": " + ex.Message;
				return false;
			}
		}

		static bool SameContent(string a, string b) {
			if (new FileInfo(a).Length != new FileInfo(b).Length) return false;
			return File.ReadAllBytes(a).SequenceEqual(File.ReadAllBytes(b));
		}

		// "Remove plugin from game": takes the plugin (and the launch cfg) out of the game folder again.
		public static string RemovePlugin(string gameDir) {
			if (!GameLocator.IsGameDir(gameDir)) return "Portal 2 wasn't found, so there is nothing to remove.";
			string dll = Path.Combine(gameDir, PluginFile), cfg = Path.Combine(gameDir, "portal2", "cfg", LaunchCfg + ".cfg");
			if (!File.Exists(dll) && !File.Exists(cfg)) return "The plugin isn't in the game folder.";
			if (App.DryRun && !App.ApplyFiles) {
				App.Log("remove plugin: would delete " + dll + " and " + cfg);
				return "Dry run: would delete " + dll + ".";
			}
			try {
				File.Delete(dll);
				File.Delete(cfg);
				if (App.DryRun) App.Log("remove plugin: deleted " + dll + " and " + cfg);
				return "Plugin removed from the game folder.";
			} catch (Exception ex) {
				if (!(ex is IOException || ex is UnauthorizedAccessException)) throw;
				string other;
				return FindGame(gameDir, out other) == GameState.Running ? "The plugin is in use: close Portal 2 first, then remove it."
				                                                          : "The plugin couldn't be removed: " + ex.Message;
			}
		}

		// ---- what is running ----

		static bool SteamRunning() {
			if (SteamRunningOverride.HasValue) return SteamRunningOverride.Value;
			return ProcessPaths("steam").Count > 0;
		}

		// Is this Portal 2 running? A portal2.exe from another folder is a mod's own copy: the game won't start
		// beside it, and it wouldn't read this game's cfg.
		static GameState FindGame(string gameDir, out string otherPath) {
			otherPath = null;
			if (GameRunningOverride.HasValue) return GameRunningOverride.Value ? GameState.Running : GameState.NotRunning;
			string exe = Path.Combine(gameDir, "portal2.exe");
			var state = GameState.NotRunning;
			foreach (var path in ProcessPaths("portal2")) {
				// (a process whose path can't be read is taken for the game)
				if (path == null || Settings.SamePath(path, exe)) return GameState.Running;
				otherPath = path;
				state = GameState.AnotherCopy;
			}
			return state;
		}

		// the exe path of every running process of that name (null for one that can't be asked)
		static List<string> ProcessPaths(string name) {
			var paths = new List<string>();
			Process[] all;
			try { all = Process.GetProcessesByName(name); } catch (InvalidOperationException) { return paths; }
			foreach (var p in all) {
				try { paths.Add(ImagePath(p.Id)); } catch (InvalidOperationException) { }
				p.Dispose();
			}
			return paths;
		}

		static string ImagePath(int pid) {
			IntPtr h = OpenProcess(0x1000 /* PROCESS_QUERY_LIMITED_INFORMATION */, false, pid);
			if (h == IntPtr.Zero) {
				if (Marshal.GetLastWin32Error() == 87 /* ERROR_INVALID_PARAMETER */) throw new InvalidOperationException("the process has ended");
				return null;
			}
			try {
				var sb = new StringBuilder(1024);
				int size = sb.Capacity;
				return QueryFullProcessImageName(h, 0, sb, ref size) ? sb.ToString(0, size) : null;
			} finally { CloseHandle(h); }
		}

		[DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(int access, bool inherit, int pid);
		[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool QueryFullProcessImageName(IntPtr process, int flags, StringBuilder name, ref int size);
		[DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
	}
}
