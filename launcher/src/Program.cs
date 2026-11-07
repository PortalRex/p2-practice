// P2 Practice launcher: lists demos, lets the player choose how to practice one, and starts Portal 2 with the
// p2practice plugin loaded and the session set up.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Text;
using System.Windows.Forms;

namespace P2Practice {
	// What the command line asked for. Everything except --dry-run is for testing without a window, e.g.
	//   P2Practice.exe --screenshot out.png --search doors --select 0 --part 2 --set role=orange --launch
	// writes a picture of the window with that run selected and Launch pressed, and (as every dry run does) what
	// Launch would have started to P2Practice.log. Nothing is shown and nothing is started.
	static class App {
		public const string Title = "P2 Practice";
		public static string ExeDir;          // where P2Practice.exe, p2route.exe and p2practice.dll live
		public static string Home;            // where the ini, the cache and the log go (the exe's folder; --home moves it)
		public static bool DryRun;            // --dry-run: Launch says what it would do instead of doing it
		public static bool ApplyFiles;        // --apply-files: with --dry-run, still install the plugin / write the cfg (tests, with --game)
		public static string GameOverride;    // --game <folder>: use this Portal 2 folder, without remembering it
		public static string Screenshot;      // --screenshot <file.png>: render the window to a file without showing it
		public static string Board;           // --board <map>: the screenshot shows that chamber's leaderboard
		public static bool Get;               // --get: fetch the selected board run's demo before the screenshot
		public static int Select = -1;        // --select <row>: the row selected in the screenshot (0 = first)
		public static int Dpi;                // --dpi <percent>: lay the screenshot out as on a monitor at this scale
		public static int Part;               // --part <number>: the part selected in the screenshot
		public static string Search;          // --search <text>: typed into the search box
		public static Size Size;              // --size <width>x<height>: window size in the screenshot (at 100%)
		public static bool PressLaunch;       // --launch: press Launch before the screenshot (always a dry run)
		public static string SaveCfgName;     // --save-cfg <name>: press Save as cfg with this name before the screenshot
		public static double TakeOver = -1;   // --take-over <seconds>: type this take-over second before the screenshot
		public static int RescaleTo;          // --rescale <percent>: then rebuild the window for this scale, as moving it to another monitor does
		public static bool SaveSettings;      // --save: the screenshot run remembers settings like a real one (otherwise it leaves them alone)
		public static bool RemovePlugin;      // --remove-plugin: choose Game > Remove plugin from game before the screenshot
		public static bool SelfTest;          // --selftest: do what the mouse and keys would do, through the controls' own handlers, and log it
		public static readonly List<KeyValuePair<string, string>> Sets = new List<KeyValuePair<string, string>>(); // --set key=value: an option, as in the ini

		public static bool Headless { get { return Screenshot != null; } }
		public static string LogPath { get { return Path.Combine(Home, "P2Practice.log"); } }

		public static void Parse(string[] args) {
			ExeDir = Path.GetDirectoryName(Application.ExecutablePath);
			Home = ExeDir;
			for (int i = 0; i < args.Length; i++) {
				string a = args[i].ToLowerInvariant();
				string next = i + 1 < args.Length ? args[i + 1] : null;
				int n;
				switch (a) {
					case "--dry-run": DryRun = true; break;
					case "--apply-files": ApplyFiles = true; break;
					case "--launch": PressLaunch = true; break;
					case "--save": SaveSettings = true; break;
					case "--remove-plugin": RemovePlugin = true; break;
					case "--selftest": SelfTest = true; break;
					case "--show-focus": SignControl.AllFocusCues = true; break; // every keyboard-focus cue drawn at once, to look at them
					case "--rescale": if (int.TryParse(next, out n)) { RescaleTo = Math.Max(50, Math.Min(400, n)); i++; } break;
					case "--home": if (next != null) { Home = Path.GetFullPath(next); i++; } break;
					case "--game": if (next != null) { GameOverride = Path.GetFullPath(next); i++; } break;
					case "--screenshot": if (next != null) { Screenshot = Path.GetFullPath(next); i++; } break;
					case "--board": if (next != null) { Board = next; i++; } break;
					case "--get": Get = true; break;
					case "--search": if (next != null) { Search = next; i++; } break;
					case "--select": if (int.TryParse(next, out n)) { Select = n; i++; } break;
					case "--dpi": if (int.TryParse(next, out n)) { Dpi = Math.Max(50, Math.Min(400, n)); i++; } break;
					case "--part": if (int.TryParse(next, out n)) { Part = n; i++; } break;
					case "--save-cfg": if (next != null) { SaveCfgName = next; i++; } break;
					case "--take-over": {
						double d;
						if (double.TryParse(next, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out d)) { TakeOver = d; i++; }
						break;
					}
					case "--game-running": if (next != null) { GameLauncher.GameRunningOverride = IsYes(next); i++; } break;
					case "--steam-running": if (next != null) { GameLauncher.SteamRunningOverride = IsYes(next); i++; } break;
					case "--size":
						if (next != null) {
							var wh = next.ToLowerInvariant().Split('x');
							int w, h;
							if (wh.Length == 2 && int.TryParse(wh[0], out w) && int.TryParse(wh[1], out h)) Size = new Size(w, h);
							i++;
						}
						break;
					case "--set":
						if (next != null) {
							int eq = next.IndexOf('=');
							if (eq > 0) Sets.Add(new KeyValuePair<string, string>(next.Substring(0, eq).Trim(), next.Substring(eq + 1).Trim()));
							i++;
						}
						break;
				}
			}
			if (Headless) DryRun = true; // a screenshot run never starts anything
		}

		static bool IsYes(string s) { return s == "1" || s.Equals("yes", StringComparison.OrdinalIgnoreCase) || s.Equals("true", StringComparison.OrdinalIgnoreCase); }

		public static void Log(string text) {
			try {
				Directory.CreateDirectory(Home);
				File.AppendAllText(LogPath, text + Environment.NewLine, new UTF8Encoding(false));
			} catch (IOException) { } catch (UnauthorizedAccessException) { }
		}
	}

	static class Program {
		public static Icon AppIcon;

		[STAThread]
		static int Main(string[] args) {
			App.Parse(args);
			Application.EnableVisualStyles();
			Application.SetCompatibleTextRenderingDefault(false);
			try { AppIcon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
			if (App.DryRun) App.Log("--- " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture) + " dry run: nothing is started ---");
			if (App.Headless) return ScreenshotRun();

			// a bug shows one clear dialog instead of the framework's crash box, and the window stays up
			Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
			Application.ThreadException += (s, e) => MessageBox.Show(e.Exception.Message + "\n\n" + e.Exception.StackTrace, App.Title + " - something went wrong",
				MessageBoxButtons.OK, MessageBoxIcon.Error);
			Application.Run(new MainForm());
			return 0;
		}

		// --screenshot: builds the window without ever showing it, lets the library load as it does on screen,
		// selects what was asked for, optionally presses Launch (a dry run), and saves a picture of the result.
		static int ScreenshotRun() {
			int code = 0;
			Application.SetUnhandledExceptionMode(UnhandledExceptionMode.ThrowException);
			try {
				using (var form = new MainForm()) {
					form.CreateHidden();
					var deadline = DateTime.UtcNow.AddMinutes(3);
					int step = 0, settle = 0;
					var timer = new Timer { Interval = 30 };
					timer.Tick += (s, e) => {
						try {
							if (DateTime.UtcNow > deadline) throw new TimeoutException("the window did not settle in time");
							if (step == 0 && form.LibraryIdle) {
								if (App.Board != null) { form.ShowBoard(App.Board); step = 10; }
								else { form.SelectRow(App.Select); step = 1; }
							}
							else if (step == 10 && form.BoardReady) { form.SelectBoardRow(App.Select); if (App.Get) { form.GetBoard(); step = 11; } else step = 2; }
							else if (step == 11 && form.Fetched && form.LibraryIdle && form.RunReady) step = 2;
							else if (step == 1 && form.RunReady) {
								if (App.SelfTest) form.SelfTest();
								if (App.Part > 0) form.SelectPart(App.Part);
								if (App.TakeOver >= 0) form.SetTakeOver(App.TakeOver);
								if (App.SaveCfgName != null) form.SaveCfgAs(App.SaveCfgName);
								if (App.PressLaunch) form.Launch();
								if (App.RemovePlugin) form.RemovePlugin();
								if (App.RescaleTo > 0) form.RescaleTo(App.RescaleTo);
								step = 2;
							} else if (step == 2 && ++settle >= 3) {
								timer.Stop();
								form.SavePicture(App.Screenshot);
								if (App.SelfTest) form.SaveMenuPicture(Path.ChangeExtension(App.Screenshot, null) + "-menu.png");
								form.Close(); // (remembers the settings, with --save)
								Application.ExitThread();
							}
						} catch (Exception ex) {
							timer.Stop();
							App.Log("screenshot failed: " + ex);
							code = 2;
							Application.ExitThread();
						}
					};
					timer.Start();
					Application.Run();
					timer.Dispose();
				}
			} catch (Exception ex) {
				App.Log("screenshot failed: " + ex);
				code = 2;
			}
			return code;
		}
	}
}
