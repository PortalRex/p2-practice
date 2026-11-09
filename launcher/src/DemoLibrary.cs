// The demos in the player's folders: found and read in the background, remembered between runs, and put together
// into the rows the window lists (a co-op run recorded by both players is one row).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;

namespace P2Practice {
	class DemoFile {
		public string Path;
		public string Name;      // file name without ".dem"
		public long Size, Modified;
		public ScanResult Scan;
	}

	// One row of the list.
	class Run {
		public DemoFile Demo;     // the only demo, or the host's of a co-op pair
		public DemoFile Partner;  // the client's demo of the same run, or null
		public string MapName, Players, Mode, Files, SearchText;

		public bool Ok { get { return Demo.Scan.Ok; } }
		public string Map { get { return Demo.Scan.Map; } }
		public bool Coop { get { return Demo.Scan.Coop; } }
		public double Time { get { return Demo.Scan.Time; } }
		// "~": the demo's length, for a demo without a run time (nothing when the demo doesn't say that either)
		public string TimeText { get { return !Ok ? "" : Demo.Scan.HasTime ? Demo.Scan.TimeText : Demo.Scan.Time > 0 ? "~" + Demo.Scan.TimeText : ""; } }
		public string Key { get { return Demo.Path + "|" + (Partner != null ? Partner.Path : ""); } }

		public Run(DemoFile demo, DemoFile partner) {
			Demo = demo;
			Partner = partner;
			var s = demo.Scan;
			Files = demo.Name + ".dem" + (partner != null ? "  +  " + partner.Name + ".dem" : "");
			if (!s.Ok) {
				MapName = "(can't be read)";
				Players = Mode = "";
			} else {
				MapName = Maps.Name(s.Map);
				Players = s.Player + (partner != null ? "  +  " + partner.Scan.Player : "");
				Mode = !s.Coop ? "SP" : partner != null ? "Co-op" : s.Host ? "Co-op (blue)" : "Co-op (orange)";
			}
			SearchText = (MapName + "\n" + s.Map + "\n" + Players + "\n" + Mode + "\n" + Files).ToLowerInvariant();
		}

		// Puts scanned demos together into rows. Two co-op demos are the same run when they are of the same map
		// and the same run time, one recorded by the host and one by the client.
		public static List<Run> Build(List<DemoFile> files) {
			var runs = new List<Run>();
			var groups = new Dictionary<string, List<DemoFile>>();
			foreach (var f in files) {
				var s = f.Scan;
				if (!s.Ok || !s.Coop || !s.HasTime) { runs.Add(new Run(f, null)); continue; }
				string key = s.Map.ToLowerInvariant() + "\n" + s.Ticks;
				List<DemoFile> g;
				if (!groups.TryGetValue(key, out g)) groups[key] = g = new List<DemoFile>();
				g.Add(f);
			}
			foreach (var g in groups.Values) {
				var hosts = g.Where(f => f.Scan.Host).OrderBy(f => f.Path, StringComparer.OrdinalIgnoreCase).ToList();
				var clients = g.Where(f => !f.Scan.Host).OrderBy(f => f.Path, StringComparer.OrdinalIgnoreCase).ToList();
				var used = new HashSet<DemoFile>();
				if (hosts.Count == 1 && clients.Count == 1) {
					runs.Add(new Run(hosts[0], clients[0]));
					used.Add(hosts[0]); used.Add(clients[0]);
				} else if (hosts.Count > 0 && clients.Count > 0 && (long)hosts.Count * clients.Count <= 250000) {
					// several runs with the same time (ties happen): each host goes with the client that looks
					// most like its other half, best matches first
					var candidates = new List<KeyValuePair<int, KeyValuePair<DemoFile, DemoFile>>>();
					foreach (var h in hosts) foreach (var c in clients)
						candidates.Add(new KeyValuePair<int, KeyValuePair<DemoFile, DemoFile>>(Likeness(h, c), new KeyValuePair<DemoFile, DemoFile>(h, c)));
					foreach (var c in candidates.OrderByDescending(c => c.Key)) { // (a stable sort: ties stay in path order)
						if (used.Contains(c.Value.Key) || used.Contains(c.Value.Value)) continue;
						runs.Add(new Run(c.Value.Key, c.Value.Value));
						used.Add(c.Value.Key); used.Add(c.Value.Value);
					}
				}
				foreach (var f in g) if (!used.Contains(f)) runs.Add(new Run(f, null)); // a co-op demo without its partner
			}
			return runs;
		}

		// How much two demos look like the two halves of one run: saved in the same folder, under names that
		// start the same and end in neighbouring numbers (doors_wr_359667 / doors_wr_359668).
		static int Likeness(DemoFile a, DemoFile b) {
			int score = 0;
			if (string.Equals(System.IO.Path.GetDirectoryName(a.Path), System.IO.Path.GetDirectoryName(b.Path), StringComparison.OrdinalIgnoreCase)) score += 1000;
			int same = 0;
			while (same < a.Name.Length && same < b.Name.Length && char.ToLowerInvariant(a.Name[same]) == char.ToLowerInvariant(b.Name[same])) same++;
			score += Math.Min(same, 200);
			long na, nb;
			if (TrailingNumber(a.Name, out na) && TrailingNumber(b.Name, out nb) && Math.Abs(na - nb) == 1) score += 100;
			return score;
		}

		static bool TrailingNumber(string name, out long n) {
			int start = name.Length;
			while (start > 0 && char.IsDigit(name[start - 1]) && name.Length - start < 18) start--;
			return long.TryParse(name.Substring(start), NumberStyles.None, CultureInfo.InvariantCulture, out n);
		}

		// The list's order: by column, then game order and time. Demos that can't be read go last.
		public static Comparison<Run> Order(int column, bool descending) {
			return (a, b) => {
				if (a.Ok != b.Ok) return a.Ok ? -1 : 1;
				int c = 0;
				switch (column) {
					case 1: c = a.Time.CompareTo(b.Time); break;
					case 2: c = string.Compare(a.Players, b.Players, StringComparison.CurrentCultureIgnoreCase); break;
					case 3: c = string.Compare(a.Mode, b.Mode, StringComparison.CurrentCultureIgnoreCase); break;
					case 4: c = string.Compare(a.Files, b.Files, StringComparison.CurrentCultureIgnoreCase); break;
				}
				if (c == 0) {
					c = Maps.Order(a.Map).CompareTo(Maps.Order(b.Map));
					if (c == 0) c = string.Compare(a.MapName, b.MapName, StringComparison.CurrentCultureIgnoreCase);
					if (column != 0 && descending) c = -c; // the tie-breakers stay ascending
				}
				if (descending) c = -c;
				if (c == 0) c = a.Time.CompareTo(b.Time);
				if (c == 0) c = string.Compare(a.Files, b.Files, StringComparison.OrdinalIgnoreCase);
				return c;
			};
		}
	}

	class DemoLibrary {
		readonly object gate = new object();
		Dictionary<string, DemoFile> cache;              // every demo read so far, by lower-case path
		List<DemoFile> files = new List<DemoFile>();     // the demos in the folders now, those read so far
		int generation;                                  // a newer Start makes older workers stop
		int version, total, done;
		bool busy, cacheDirty;
		string problem;

		static readonly object cacheFile = new object(); // one writer of the cache file at a time
		static string CachePath { get { return Path.Combine(App.Home, "P2Practice.cache"); } }

		public int Version { get { lock (gate) return version; } }   // changes whenever Snapshot would differ
		public bool Busy { get { lock (gate) return busy; } }
		public string TakeProblem() { lock (gate) { var p = problem; problem = null; return p; } } // something went wrong reading the folders (once)
		public void Progress(out int doneCount, out int totalCount) { lock (gate) { doneCount = done; totalCount = total; } }
		public List<DemoFile> Snapshot() { lock (gate) return new List<DemoFile>(files); }

		// Forgets the demos the helper was stopped on for taking too long, so that the next Start tries them again.
		public void ForgetSlow() {
			lock (gate) {
				if (cache == null) return;
				var slow = cache.Where(e => e.Value.Scan.Error == RouteCli.TooSlowMessage).Select(e => e.Key).ToList();
				foreach (var key in slow) cache.Remove(key);
				if (slow.Count > 0) cacheDirty = true;
			}
		}

		// Lists the folders' demos and reads the ones not read before. Returns at once; watch Version.
		public void Start(IEnumerable<string> folders) {
			var list = folders.ToList();
			int mine;
			lock (gate) { mine = ++generation; busy = true; total = done = 0; version++; }
			var t = new Thread(() => Work(mine, list)) { IsBackground = true, Name = "demo library", Priority = ThreadPriority.BelowNormal };
			t.Start();
		}

		bool Current(int mine) { lock (gate) return mine == generation; }

		void Work(int mine, List<string> folders) {
			try {
				lock (gate) if (cache == null) cache = LoadCache();

				// 1. what is there
				var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
				var ready = new List<DemoFile>();
				var pending = new List<DemoFile>();
				var missing = new List<string>();
				foreach (var folder in folders) {
					if (!Current(mine)) return;
					if (!SafeExists(folder)) { missing.Add(folder); continue; }
					foreach (var path in DemosIn(folder)) {
						if (!seen.Add(path)) continue; // a folder inside another listed folder
						var f = new DemoFile { Path = path, Name = Path.GetFileNameWithoutExtension(path) };
						try {
							var info = new FileInfo(path);
							f.Size = info.Length;
							f.Modified = info.LastWriteTimeUtc.Ticks;
						} catch (IOException) { continue; } catch (UnauthorizedAccessException) { continue; }
						DemoFile known;
						lock (gate) cache.TryGetValue(path.ToLowerInvariant(), out known);
						if (known != null && known.Size == f.Size && known.Modified == f.Modified) { f.Scan = known.Scan; ready.Add(f); }
						else pending.Add(f);
					}
				}
				lock (gate) {
					if (mine != generation) return;
					files = ready;
					total = pending.Count;
					version++;
				}

				// 2. read the new ones, a few at a time
				bool helper = RouteCli.Exists;
				int next = -1;
				ThreadStart reader = () => {
					while (true) {
						int i = Interlocked.Increment(ref next);
						if (i >= pending.Count || !Current(mine)) return;
						var f = pending[i];
						try { f.Scan = helper ? RouteCli.Scan(f.Path) : new ScanResult { Error = RouteCli.MissingMessage, Transient = true }; }
						catch (Exception ex) { f.Scan = new ScanResult { Error = ex.Message, Transient = true }; } // (nothing a demo does may take the launcher down)
						lock (gate) {
							if (mine != generation) return;
							files.Add(f);
							if (!f.Scan.Transient) { cache[f.Path.ToLowerInvariant()] = f; cacheDirty = true; }
							done++;
							version++;
						}
					}
				};
				var readers = new List<Thread>();
				int count = helper ? Math.Min(pending.Count, Math.Max(1, Math.Min(4, Environment.ProcessorCount / 2))) : Math.Min(pending.Count, 1);
				for (int i = 0; i < count; i++) {
					var t = new Thread(reader) { IsBackground = true, Name = "demo reader", Priority = ThreadPriority.BelowNormal };
					t.Start();
					readers.Add(t);
				}
				// (a long first read of a big library is saved as it goes, so closing the window loses little)
				while (readers.Any(t => !t.Join(10000))) SaveCache();
				if (!Current(mine)) return;

				// 3. forget demos that are gone - but not those of a folder that is only away for now (a drive not plugged in)
				lock (gate) {
					var keep = new Dictionary<string, DemoFile>();
					foreach (var f in files) if (!f.Scan.Transient) keep[f.Path.ToLowerInvariant()] = f;
					foreach (var e in cache)
						if (!keep.ContainsKey(e.Key) && missing.Any(m => e.Key.StartsWith(m.TrimEnd('\\', '/').ToLowerInvariant() + "\\", StringComparison.Ordinal))) keep[e.Key] = e.Value;
					if (keep.Count != cache.Count) cacheDirty = true;
					cache = keep;
				}
				SaveCache();
			} catch (Exception ex) {
				lock (gate) problem = "The demo folders couldn't be read: " + ex.Message; // (shown in the status line)
			} finally {
				lock (gate) if (mine == generation) { busy = false; version++; }
			}
		}

		static bool SafeExists(string folder) {
			try { return Directory.Exists(folder); } catch (ArgumentException) { return false; }
		}

		// Every .dem under a folder. Folders that can't be opened are skipped, links to folders aren't followed
		// (they can loop).
		static IEnumerable<string> DemosIn(string root) {
			var todo = new Stack<string>();
			try { todo.Push(Path.GetFullPath(root)); } catch (ArgumentException) { } catch (NotSupportedException) { } catch (PathTooLongException) { }
			while (todo.Count > 0) {
				string dir = todo.Pop();
				string[] found = null, subs = null;
				try {
					found = Directory.GetFiles(dir, "*.dem");
					subs = Directory.GetDirectories(dir);
				} catch (IOException) { } catch (UnauthorizedAccessException) { }
				if (found != null)
					foreach (var f in found)
						if (f.EndsWith(".dem", StringComparison.OrdinalIgnoreCase)) yield return f; // ("*.dem" also matches .demo, .dem_old...)
				if (subs == null) continue;
				foreach (var s in subs) {
					try {
						if ((File.GetAttributes(s) & FileAttributes.ReparsePoint) == 0) todo.Push(s);
					} catch (IOException) { } catch (UnauthorizedAccessException) { }
				}
			}
		}

		// ---- the cache: one line per demo, tab-separated, so reopening the launcher asks the helper nothing ----

		const string CacheHeader = "P2PracticeCache 1";

		static Dictionary<string, DemoFile> LoadCache() {
			var d = new Dictionary<string, DemoFile>();
			try {
				var lines = File.ReadAllLines(CachePath, Encoding.UTF8);
				// answers from another build of the helper are asked for again
				if (lines.Length == 0 || lines[0] != CacheHeader + "\t" + RouteCli.Stamp) return d;
				for (int i = 1; i < lines.Length; i++) {
					var p = lines[i].Split('\t');
					if (p.Length < 13) continue;
					var f = new DemoFile { Path = Unescape(p[0]), Scan = new ScanResult() };
					f.Name = Path.GetFileNameWithoutExtension(f.Path);
					var s = f.Scan;
					if (!long.TryParse(p[1], out f.Size) || !long.TryParse(p[2], out f.Modified)) continue;
					s.Ok = p[3] == "1";
					s.Map = Unescape(p[4]);
					s.Coop = p[5] == "1";
					s.Player = Unescape(p[6]);
					s.Host = p[7] == "1";
					int.TryParse(p[8], NumberStyles.Integer, CultureInfo.InvariantCulture, out s.Ticks);
					double.TryParse(p[9], NumberStyles.Float, CultureInfo.InvariantCulture, out s.Time);
					s.TimeText = Unescape(p[10]);
					s.HasTime = p[11] == "1";
					s.Error = p[12].Length > 0 ? Unescape(p[12]) : null;
					d[f.Path.ToLowerInvariant()] = f;
				}
			} catch (IOException) { } catch (UnauthorizedAccessException) { } catch (ArgumentException) { }
			return d;
		}

		// Writes the cache if it changed. (Called from the reading thread of every Start, and an older one can still
		// be finishing when the next begins: they take turns.)
		void SaveCache() {
			lock (cacheFile) {
				List<DemoFile> all;
				lock (gate) {
					if (!cacheDirty || cache == null) return;
					cacheDirty = false;
					all = cache.Values.ToList();
				}
				if (!WriteCache(all)) lock (gate) cacheDirty = true; // (still to be saved: the next call tries again)
			}
		}

		static bool WriteCache(List<DemoFile> all) {
			var sb = new StringBuilder();
			sb.Append(CacheHeader).Append('\t').Append(RouteCli.Stamp).Append('\n');
			foreach (var f in all.OrderBy(f => f.Path, StringComparer.OrdinalIgnoreCase)) {
				var s = f.Scan;
				sb.Append(Escape(f.Path)).Append('\t').Append(f.Size).Append('\t').Append(f.Modified).Append('\t')
				  .Append(s.Ok ? '1' : '0').Append('\t').Append(Escape(s.Map)).Append('\t').Append(s.Coop ? '1' : '0').Append('\t')
				  .Append(Escape(s.Player)).Append('\t').Append(s.Host ? '1' : '0').Append('\t')
				  .Append(s.Ticks.ToString(CultureInfo.InvariantCulture)).Append('\t').Append(s.Time.ToString("R", CultureInfo.InvariantCulture)).Append('\t')
				  .Append(Escape(s.TimeText)).Append('\t').Append(s.HasTime ? '1' : '0').Append('\t').Append(Escape(s.Error ?? "")).Append('\n');
			}
			try {
				Directory.CreateDirectory(App.Home);
				string tmp = CachePath + ".tmp";
				File.WriteAllText(tmp, sb.ToString(), new UTF8Encoding(false));
				// (swapped in one step: there is no moment without a cache file)
				if (File.Exists(CachePath)) File.Replace(tmp, CachePath, null);
				else File.Move(tmp, CachePath);
				return true;
			} catch (IOException) { return false; } catch (UnauthorizedAccessException) { return false; }
		}

		static string Escape(string s) {
			return (s ?? "").Replace("\\", "\\\\").Replace("\t", "\\t").Replace("\n", "\\n").Replace("\r", "\\r");
		}

		static string Unescape(string s) {
			if (s.IndexOf('\\') < 0) return s;
			var sb = new StringBuilder(s.Length);
			for (int i = 0; i < s.Length; i++) {
				if (s[i] != '\\' || i + 1 >= s.Length) { sb.Append(s[i]); continue; }
				char c = s[++i];
				sb.Append(c == 't' ? '\t' : c == 'n' ? '\n' : c == 'r' ? '\r' : c);
			}
			return sb.ToString();
		}
	}
}
