// A stand-in for the real p2route.exe, for developing and testing the launcher. It answers the same two calls
// with the same JSON:
//   p2route.exe scan <demo>
//   p2route.exe info <demo> [partner demo]
// What it reads for real: the demo's header (map, player, host or client) and the SAR run time, if the demo
// has one. What it makes up: the parts of the run.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

static class P2RouteStub {
	class Demo {
		public string Map, Player, Server;
		public bool Coop, Host, HasTime;
		public int Ticks;
		public double Time;
		public string TimeText { get { return Format(Time); } }
	}

	static int Main(string[] args) {
		try {
			if (args.Length >= 2 && args[0] == "scan") return Print(Scan(Read(args[1])));
			if (args.Length >= 2 && args[0] == "info") return Print(Info(Read(args[1]), args.Length >= 3 ? Read(args[2]) : null));
			return Print("{\"ok\":false,\"error\":\"usage: p2route scan <demo> | info <demo> [partner demo]\"}", 1);
		} catch (Exception ex) {
			return Print("{\"ok\":false,\"error\":" + Q(ex.Message) + "}", 1);
		}
	}

	static int Print(string json, int code = 0) {
		var bytes = new UTF8Encoding(false).GetBytes(json + "\n");
		using (var o = Console.OpenStandardOutput()) o.Write(bytes, 0, bytes.Length);
		return code;
	}

	static string Scan(Demo d) {
		return "{\"ok\":true,\"map\":" + Q(d.Map) + ",\"coop\":" + B(d.Coop) + ",\"player\":" + Q(d.Player) + ",\"host\":" + B(d.Host)
			+ ",\"ticks\":" + d.Ticks + ",\"time\":" + N(d.Time) + ",\"timeText\":" + Q(d.TimeText) + ",\"hasTime\":" + B(d.HasTime) + "}";
	}

	static readonly string[] SpParts = { "Start", "First portal", "Through the door", "Fling across the gap", "Cube on the button", "Up to the ledge", "Exit door", "Into the elevator" };
	static readonly string[] CoopParts = { "Start", "Orange places first portal on the ceiling", "Blue goes through", "Both on the buttons", "Blue holds the door",
		"Orange portals to the exit", "Through the door together", "Disassembler" };

	static string Info(Demo d, Demo partner) {
		if (partner != null && !string.Equals(partner.Map, d.Map, StringComparison.OrdinalIgnoreCase)) throw new Exception("the two demos are of different maps");
		var sb = new StringBuilder();
		sb.Append("{\"ok\":true,\"map\":").Append(Q(d.Map)).Append(",\"coop\":").Append(B(d.Coop)).Append(",\"ticks\":").Append(d.Ticks)
		  .Append(",\"time\":").Append(N(d.Time)).Append(",\"timeText\":").Append(Q(d.TimeText)).Append(",\"players\":[");
		if (!d.Coop) sb.Append(Player("", d.Player, true));
		else {
			// the host plays ATLAS (blue), the client P-body (orange)
			Demo blue = d.Host ? d : partner, orange = d.Host ? partner : d;
			sb.Append(Player("blue", blue != null ? blue.Player : "Partner", blue != null)).Append(',')
			  .Append(Player("orange", orange != null ? orange.Player : "Partner", orange != null));
		}
		sb.Append("],\"parts\":[");
		var names = d.Coop ? CoopParts : SpParts;
		int count = Math.Max(2, Math.Min(names.Length, (int)Math.Round(d.Time / 1.2)));
		double lead = 2.3, each = Math.Max(0.1, d.Time / count);
		for (int i = 0; i < count; i++) {
			if (i > 0) sb.Append(',');
			sb.Append("{\"index\":").Append(i + 1).Append(",\"start\":").Append(N(Math.Round(lead + i * each, 2)))
			  .Append(",\"end\":").Append(N(Math.Round(lead + (i + 1) * each, 2))).Append(",\"name\":").Append(Q(names[i])).Append('}');
		}
		sb.Append("],\"notes\":[\"Made-up parts: this p2route.exe is the launcher's development stand-in.\"]}");
		return sb.ToString();
	}

	static string Player(string team, string name, bool hasInputs) {
		return "{\"team\":" + Q(team) + ",\"name\":" + Q(name) + ",\"hasInputs\":" + B(hasInputs) + "}";
	}

	// ---- the demo file ----

	const int HeaderSize = 1072;

	static Demo Read(string path) {
		byte[] data = File.ReadAllBytes(path);
		if (data.Length < HeaderSize || Encoding.ASCII.GetString(data, 0, 7) != "HL2DEMO") throw new Exception("not a Source engine demo");
		var d = new Demo();
		d.Server = Text(data, 16);
		d.Player = Text(data, 276);
		d.Map = Text(data, 536);
		d.Time = BitConverter.ToSingle(data, 1056);
		d.Ticks = BitConverter.ToInt32(data, 1060);
		d.Coop = d.Map.StartsWith("mp_coop", StringComparison.OrdinalIgnoreCase);
		d.Host = d.Server.StartsWith("localhost", StringComparison.OrdinalIgnoreCase); // a client's demo names the host's address
		if (d.Map.Length == 0) throw new Exception("the demo names no map");
		int runTicks = SarRunTicks(data);
		if (runTicks > 0) { d.HasTime = true; d.Ticks = runTicks; d.Time = runTicks / 60.0; }
		if (float.IsNaN((float)d.Time) || d.Time < 0) d.Time = 0;
		return d;
	}

	static string Text(byte[] data, int offset) {
		int end = offset;
		while (end < offset + 260 && data[end] != 0) end++;
		return Encoding.UTF8.GetString(data, offset, end - offset);
	}

	// Walks the demo's messages for SAR's "speedrun finished" record and returns the run's length in ticks (0: none).
	static int SarRunTicks(byte[] data) {
		int found = 0;
		try {
			int pos = HeaderSize;
			while (pos + 6 <= data.Length) {
				byte type = data[pos];
				pos += 6; // type, tick, slot
				if (type == 7) break; // stop
				if (type == 3) continue; // sync tick
				if (type == 1 || type == 2) pos += 152 + 8; // two players' view info, sequence numbers
				else if (type == 5 || type == 8) pos += 4; // command number / callback index
				else if (type != 4 && type != 6 && type != 9) break;
				int size = BitConverter.ToInt32(data, pos);
				pos += 4;
				if (size < 0 || pos + size > data.Length) break;
				// SAR's records ride in custom data: 8 bytes of cursor position, a type byte, the record
				if (type == 8 && size > 13 && data[pos + 8] == 0x0A) {
					int ticks = RunTicks(data, pos + 9, pos + size);
					if (ticks > 0) found = ticks;
				}
				pos += size;
			}
		} catch (ArgumentException) { } catch (IndexOutOfRangeException) { }
		return found;
	}

	static int RunTicks(byte[] data, int pos, int end) {
		long total = 0;
		int splits = BitConverter.ToInt32(data, pos); pos += 4;
		if (splits < 0 || splits > 10000) return 0;
		for (int i = 0; i < splits; i++) {
			pos = SkipString(data, pos, end);
			int segments = BitConverter.ToInt32(data, pos); pos += 4;
			if (segments < 0 || segments > 10000) return 0;
			for (int j = 0; j < segments; j++) {
				pos = SkipString(data, pos, end);
				total += BitConverter.ToInt32(data, pos); pos += 4;
			}
		}
		return total > 0 && total < int.MaxValue ? (int)total : 0;
	}

	static int SkipString(byte[] data, int pos, int end) {
		while (pos < end && data[pos] != 0) pos++;
		if (pos >= end) throw new IndexOutOfRangeException();
		return pos + 1;
	}

	// ---- JSON ----

	static string B(bool b) { return b ? "true" : "false"; }
	static string N(double d) { return d.ToString("0.####", CultureInfo.InvariantCulture); }

	static string Format(double seconds) {
		long cs = (long)Math.Floor(seconds * 100 + 1e-6);
		return cs >= 6000 ? (cs / 6000) + ":" + ((cs % 6000) / 100.0).ToString("00.00", CultureInfo.InvariantCulture)
		                  : (cs / 100.0).ToString("0.00", CultureInfo.InvariantCulture);
	}

	static string Q(string s) {
		var sb = new StringBuilder("\"");
		foreach (char c in s ?? "") {
			if (c == '"' || c == '\\') sb.Append('\\').Append(c);
			else if (c < 32) sb.Append("\\u").Append(((int)c).ToString("x4"));
			else sb.Append(c);
		}
		return sb.Append('"').ToString();
	}
}
