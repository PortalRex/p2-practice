// demodump <file.dem> [--no-entities] [--no-movers]
//
// Prints everything p2demo::ParseFile reads from a demo as one JSON object on
// stdout (the same layout tools/tsdump.ts prints from the TypeScript
// parser; tools/compare.mjs compares the two).  Exit code 1 and a message on
// stderr when the file can't be read as a demo.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "p2demo/demo.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace {

using namespace p2demo;

// How a string's bytes map to characters.  The TypeScript parser decodes
// most text as UTF-8 and string table names (models, classes) byte by byte.
enum class Text { UTF8, LATIN1 };

class JsonWriter {
public:
	const std::string &text() const { return out_; }

	void beginObject() { open('{'); }
	void endObject() { close('}'); }
	void beginArray() { open('['); }
	void endArray() { close(']'); }

	void key(const char *name) {
		comma();
		out_ += '"';
		out_ += name;
		out_ += "\":";
		needComma_ = false;
	}

	void value(int v) { number("%d", v); }
	void value(unsigned v) { number("%u", v); }
	void value(bool v) {
		comma();
		out_ += v ? "true" : "false";
	}
	void value(float v) {
		if (!std::isfinite(v)) {
			comma();
			out_ += "null";
		} else {
			number("%.9g", double(v));
		}
	}
	void value(const Vec3 &v) {
		beginArray();
		value(v.x);
		value(v.y);
		value(v.z);
		endArray();
	}
	void value(const std::string &s, Text text = Text::UTF8) {
		comma();
		out_ += '"';
		if (text == Text::LATIN1) {
			for (const char c : s) appendCodePoint(uint8_t(c));
		} else {
			appendUtf8(s);
		}
		out_ += '"';
	}

	template <typename T>
	void field(const char *name, const T &v) {
		key(name);
		value(v);
	}
	void field(const char *name, const std::string &s, Text text) {
		key(name);
		value(s, text);
	}

private:
	void open(char c) {
		comma();
		out_ += c;
		needComma_ = false;
	}
	void close(char c) {
		out_ += c;
		needComma_ = true;
	}
	void comma() {
		if (needComma_) out_ += ',';
		needComma_ = true;
	}
	template <typename T>
	void number(const char *format, T v) {
		comma();
		char buffer[40];
		const int length = std::snprintf(buffer, sizeof buffer, format, v);
		if (length > 0) out_.append(buffer, size_t(length));
	}

	void appendCodePoint(uint32_t c) {
		if (c == '"' || c == '\\') {
			out_ += '\\';
			out_ += char(c);
		} else if (c < 0x20) {
			char buffer[8];
			std::snprintf(buffer, sizeof buffer, "\\u%04x", unsigned(c));
			out_ += buffer;
		} else if (c < 0x80) {
			out_ += char(c);
		} else if (c < 0x800) {
			out_ += char(0xc0 | (c >> 6));
			out_ += char(0x80 | (c & 0x3f));
		} else if (c < 0x10000) {
			out_ += char(0xe0 | (c >> 12));
			out_ += char(0x80 | ((c >> 6) & 0x3f));
			out_ += char(0x80 | (c & 0x3f));
		} else {
			out_ += char(0xf0 | (c >> 18));
			out_ += char(0x80 | ((c >> 12) & 0x3f));
			out_ += char(0x80 | ((c >> 6) & 0x3f));
			out_ += char(0x80 | (c & 0x3f));
		}
	}

	// The WHATWG UTF-8 decoder (what TextDecoder does): each maximal invalid
	// sequence becomes one U+FFFD.
	void appendUtf8(const std::string &s) {
		constexpr uint32_t kReplacement = 0xfffd;
		uint32_t codePoint = 0;
		int needed = 0, seen = 0;
		uint32_t lower = 0x80, upper = 0xbf;
		for (size_t i = 0; i < s.size(); i++) {
			const uint32_t b = uint8_t(s[i]);
			if (needed == 0) {
				if (b <= 0x7f) {
					appendCodePoint(b);
				} else if (b >= 0xc2 && b <= 0xdf) {
					needed = 1;
					codePoint = b & 0x1f;
				} else if (b >= 0xe0 && b <= 0xef) {
					if (b == 0xe0) lower = 0xa0;
					if (b == 0xed) upper = 0x9f;
					needed = 2;
					codePoint = b & 0x0f;
				} else if (b >= 0xf0 && b <= 0xf4) {
					if (b == 0xf0) lower = 0x90;
					if (b == 0xf4) upper = 0x8f;
					needed = 3;
					codePoint = b & 0x07;
				} else {
					appendCodePoint(kReplacement);
				}
				continue;
			}
			if (b < lower || b > upper) {
				// not a continuation: the sequence so far is invalid, and this byte starts over
				codePoint = 0;
				needed = seen = 0;
				lower = 0x80;
				upper = 0xbf;
				appendCodePoint(kReplacement);
				i--;
				continue;
			}
			lower = 0x80;
			upper = 0xbf;
			codePoint = (codePoint << 6) | (b & 0x3f);
			if (++seen != needed) continue;
			appendCodePoint(codePoint);
			codePoint = 0;
			needed = seen = 0;
		}
		if (needed != 0) appendCodePoint(kReplacement);
	}

	std::string out_;
	bool needComma_ = false;
};

void WriteDemo(JsonWriter &j, const Demo &demo) {
	j.beginObject();

	j.key("header");
	j.beginObject();
	j.field("server", demo.header.server);
	j.field("client", demo.header.client);
	j.field("map", demo.header.map);
	j.field("gameDir", demo.header.gameDir);
	j.field("demoProtocol", demo.header.demoProtocol);
	j.field("networkProtocol", demo.header.networkProtocol);
	j.field("playbackTime", demo.header.playbackTime);
	j.field("playbackTicks", demo.header.playbackTicks);
	j.field("playbackFrames", demo.header.playbackFrames);
	j.endObject();

	j.field("intervalPerTick", demo.intervalPerTick);
	j.field("firstTick", demo.firstTick);
	j.field("lastTick", demo.lastTick);
	j.field("recorderEntIndex", demo.recorderEntIndex);
	j.field("serverTickAtZero", demo.serverTickAtZero);

	j.key("views");
	j.beginArray();
	for (const View &v : demo.views) {
		j.beginObject();
		j.field("tick", v.tick);
		j.field("origin", v.origin);
		j.field("angles", v.angles);
		j.endObject();
	}
	j.endArray();

	j.key("userCmds");
	j.beginArray();
	for (const UserCmd &c : demo.userCmds) {
		j.beginObject();
		j.field("tick", c.tick);
		j.field("slot", c.slot);
		// (the 32-bit fields as the unsigned values the file holds)
		j.field("commandNumber", unsigned(c.commandNumber));
		j.field("viewAngles", c.viewAngles);
		j.field("forwardMove", c.forwardMove);
		j.field("sideMove", c.sideMove);
		j.field("upMove", c.upMove);
		j.field("buttons", unsigned(c.buttons));
		j.field("impulse", c.impulse);
		j.field("mouseDx", c.mouseDx);
		j.field("mouseDy", c.mouseDy);
		j.endObject();
	}
	j.endArray();

	j.key("consoleCmds");
	j.beginArray();
	for (const ConsoleCmd &c : demo.consoleCmds) {
		j.beginObject();
		j.field("tick", c.tick);
		j.field("slot", c.slot);
		j.field("text", c.text);
		j.endObject();
	}
	j.endArray();

	j.field("hasSarData", demo.hasSarData);

	j.key("sarPortals");
	j.beginArray();
	for (const SarPortal &p : demo.sarPortals) {
		j.beginObject();
		j.field("tick", p.tick);
		j.field("slot", p.slot);
		j.field("orange", p.orange);
		j.field("pos", p.pos);
		j.endObject();
	}
	j.endArray();

	j.key("sarEntityInputs");
	j.beginArray();
	for (const SarEntityInput &e : demo.sarEntityInputs) {
		j.beginObject();
		j.field("tick", e.tick);
		j.field("slot", e.slot);
		j.field("target", e.target);
		j.field("className", e.className);
		j.field("input", e.input);
		j.field("param", e.param);
		j.endObject();
	}
	j.endArray();

	j.key("sarFlags");
	j.beginArray();
	for (const SarFlags &f : demo.sarFlags) {
		j.beginObject();
		j.field("tick", f.tick);
		j.field("slot", f.slot);
		j.endObject();
	}
	j.endArray();

	j.key("sarSpeedruns");
	j.beginArray();
	for (const SarSpeedrun &s : demo.sarSpeedruns) {
		j.beginObject();
		j.field("tick", s.tick);
		j.field("complete", s.complete);
		j.field("totalTicks", s.totalTicks);
		j.key("splits");
		j.beginArray();
		for (const SarSplit &split : s.splits) {
			j.beginObject();
			j.field("name", split.name);
			j.field("ticks", split.ticks);
			j.endObject();
		}
		j.endArray();
		j.endObject();
	}
	j.endArray();

	j.key("sarTimescales");
	j.beginArray();
	for (const SarTimescale &t : demo.sarTimescales) {
		j.beginObject();
		j.field("tick", t.tick);
		j.field("timescale", t.timescale);
		j.endObject();
	}
	j.endArray();

	j.field("entitiesDecoded", demo.entitiesDecoded);

	j.key("players");
	j.beginArray();
	for (const PlayerTrack &p : demo.players) {
		j.beginObject();
		j.field("entIndex", p.entIndex);
		j.field("name", p.name);
		j.field("model", p.model, Text::LATIN1);
		j.field("isRecorder", p.isRecorder);
		j.field("challenge", p.challenge);
		j.key("keys");
		j.beginArray();
		for (const PlayerKey &k : p.keys) {
			j.beginObject();
			j.field("tick", k.tick);
			j.field("origin", k.origin);
			j.field("eyePitch", k.eyePitch);
			j.field("eyeYaw", k.eyeYaw);
			j.field("viewOffsetZ", k.viewOffsetZ);
			j.field("flags", k.flags);
			j.field("hasVelocity", k.hasVelocity);
			j.field("velocity", k.velocity);
			j.field("team", k.team);
			j.field("heldEntity", k.heldEntity);
			j.field("inSnapshot", k.inSnapshot);
			j.endObject();
		}
		j.endArray();
		j.endObject();
	}
	j.endArray();

	j.key("portals");
	j.beginArray();
	for (const PortalTrack &p : demo.portals) {
		j.beginObject();
		j.field("entIndex", p.entIndex);
		j.field("isPortal2", p.isPortal2);
		j.key("keys");
		j.beginArray();
		for (const PortalKey &k : p.keys) {
			j.beginObject();
			j.field("tick", k.tick);
			j.field("active", k.active);
			j.field("origin", k.origin);
			j.field("angles", k.angles);
			j.field("linkageGroup", k.linkageGroup);
			j.field("team", k.team);
			j.field("firedBy", k.firedBy);
			j.field("linkedPortal", k.linkedPortal);
			j.field("parent", k.parent);
			j.field("inSnapshot", k.inSnapshot);
			j.field("removed", k.removed);
			j.endObject();
		}
		j.endArray();
		j.endObject();
	}
	j.endArray();

	j.key("movers");
	j.beginArray();
	for (const MoverTrack &m : demo.movers) {
		j.beginObject();
		j.field("entIndex", m.entIndex);
		j.field("className", m.className, Text::LATIN1);
		j.field("model", m.model, Text::LATIN1);
		j.key("keys");
		j.beginArray();
		for (const MoverKey &k : m.keys) {
			j.beginObject();
			j.field("tick", k.tick);
			j.field("origin", k.origin);
			j.field("angles", k.angles);
			j.field("parent", k.parent);
			j.field("inSnapshot", k.inSnapshot);
			j.field("removed", k.removed);
			j.field("noDraw", k.noDraw);
			j.endObject();
		}
		j.endArray();
		j.endObject();
	}
	j.endArray();

	j.key("portalShots");
	j.beginArray();
	for (const PortalShot &s : demo.portalShots) {
		j.beginObject();
		j.field("tick", s.tick);
		j.field("shooter", s.shooter);
		j.field("team", s.team);
		j.field("portalNum", s.portalNum);
		j.field("result", s.result);
		j.field("origin", s.origin);
		j.field("angles", s.angles);
		j.endObject();
	}
	j.endArray();

	j.key("warnings");
	j.beginArray();
	for (const std::string &w : demo.warnings) j.value(w);
	j.endArray();

	j.endObject();
}

int Run(const std::vector<std::string> &args) {
	std::string path;
	ParseOptions options;
	bool usageError = false;
	for (size_t i = 1; i < args.size(); i++) {
		if (args[i] == "--no-entities") options.entities = false;
		else if (args[i] == "--no-movers") options.movers = false;
		else if (path.empty() && !args[i].empty() && args[i][0] != '-') path = args[i];
		else usageError = true;
	}
	if (usageError || path.empty()) {
		std::fprintf(stderr, "usage: demodump <file.dem> [--no-entities] [--no-movers]\n");
		return 2;
	}

	Demo demo;
	std::string error;
	if (!ParseFile(path.c_str(), demo, error, options)) {
		std::fprintf(stderr, "demodump: %s\n", error.c_str());
		return 1;
	}
	JsonWriter json;
	WriteDemo(json, demo);
	std::fwrite(json.text().data(), 1, json.text().size(), stdout);
	std::fputc('\n', stdout);
	return std::fflush(stdout) == 0 ? 0 : 1;
}

}  // namespace

#ifdef _WIN32
// (wide arguments, passed on as UTF-8: file names needn't fit the ANSI code page)
int wmain(int argc, wchar_t **argv) {
	_setmode(_fileno(stdout), _O_BINARY);
	std::vector<std::string> args;
	for (int i = 0; i < argc; i++) {
		const int length = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
		std::string arg(size_t(length > 0 ? length : 1), '\0');
		if (length > 0) WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, &arg[0], length, nullptr, nullptr);
		arg.resize(std::strlen(arg.c_str()));
		args.push_back(arg);
	}
	return Run(args);
}
#else
int main(int argc, char **argv) {
	return Run(std::vector<std::string>(argv, argv + argc));
}
#endif
