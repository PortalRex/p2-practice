// demostress [--truncations N] [--flips N] [--seed S] <file.dem>...
// demostress --time [--no-movers] <file.dem>...
// demostress --threads N <file.dem>...
//
// Robustness test for p2demo::Parse.  For every demo: N copies cut at random
// offsets, N copies with a few random bytes changed (half of them within the
// first 64 KB, where the header, the data tables and the string tables are)
// and N/4 copies both cut and changed.  Each copy is its own heap block of
// exactly its size, so an AddressSanitizer build (-DP2_ASAN=ON) reports any
// read outside it.  Any crash ends the process; a parse that takes too long
// is reported and fails the run.
//
// --time: no corruption, just the parse time of each file (best of 10);
// with --no-movers, as ParseOptions::movers = false.
// --threads N: no corruption; parses every file on N threads at once, five
// times each, and checks that each result equals the one parsed alone.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "p2demo/demo.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kSlowParseMs = 5000;

double MillisecondsSince(Clock::time_point start) {
	return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// xorshift64*: the same variants on every run with the same seed
class Random {
public:
	explicit Random(uint64_t seed) : state_(seed ? seed : 0x9e3779b97f4a7c15ull) {}
	uint64_t next() {
		state_ ^= state_ >> 12;
		state_ ^= state_ << 25;
		state_ ^= state_ >> 27;
		return state_ * 0x2545f4914f6cdd1dull;
	}
	size_t below(size_t n) { return n ? size_t(next() % n) : 0; }

private:
	uint64_t state_;
};

bool ReadFile(const std::string &path, std::vector<uint8_t> &bytes) {
	FILE *file = nullptr;
#ifdef _WIN32
	if (fopen_s(&file, path.c_str(), "rb") != 0) file = nullptr;
#else
	file = std::fopen(path.c_str(), "rb");
#endif
	if (!file) return false;
	std::fseek(file, 0, SEEK_END);
	const long size = std::ftell(file);
	std::fseek(file, 0, SEEK_SET);
	bytes.resize(size > 0 ? size_t(size) : 0);
	const bool ok = bytes.empty() || std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
	std::fclose(file);
	return ok;
}

struct Totals {
	size_t parses = 0;
	size_t accepted = 0;         // Parse returned true
	size_t entitiesDecoded = 0;
	size_t slow = 0;
	double slowestMs = 0;
	std::string slowest;
};

// Parses a copy held in a heap block of exactly `size` bytes.
void ParseCopy(const uint8_t *data, size_t size, bool movers, const std::string &what, Totals &totals) {
	std::unique_ptr<uint8_t[]> copy(new uint8_t[size ? size : 1]);
	if (size) std::memcpy(copy.get(), data, size);

	p2demo::Demo demo;
	std::string error;
	p2demo::ParseOptions options;
	options.movers = movers;
	const Clock::time_point start = Clock::now();
	const bool ok = p2demo::Parse(copy.get(), size, demo, error, options);
	const double ms = MillisecondsSince(start);

	totals.parses++;
	if (ok) totals.accepted++;
	if (demo.entitiesDecoded) totals.entitiesDecoded++;
	if (!ok && error.empty()) {
		std::printf("  FAILED without a message: %s\n", what.c_str());
		totals.slow++;
	}
	if (ms > totals.slowestMs) {
		totals.slowestMs = ms;
		totals.slowest = what;
	}
	if (ms > kSlowParseMs) {
		std::printf("  SLOW (%.0f ms): %s\n", ms, what.c_str());
		totals.slow++;
	}
}

void FlipBytes(std::vector<uint8_t> &bytes, size_t limit, Random &random) {
	if (limit > bytes.size()) limit = bytes.size();
	if (limit == 0) return;
	const size_t count = 1 + random.below(8);
	for (size_t i = 0; i < count; i++) {
		const size_t at = random.below(limit);
		// a random value, or one bit, or the extremes lengths and counts break on
		switch (random.below(4)) {
		case 0: bytes[at] = uint8_t(random.next()); break;
		case 1: bytes[at] ^= uint8_t(1u << random.below(8)); break;
		case 2: bytes[at] = 0xff; break;
		default: bytes[at] = 0x00; break;
		}
	}
}

void Stress(const std::string &path, const std::vector<uint8_t> &original, size_t truncations, size_t flips, Random &random, Totals &totals) {
	const size_t size = original.size();
	for (size_t i = 0; i < truncations; i++) {
		// (a tenth of the cuts fall inside the header and the first frames)
		const size_t cut = i % 10 == 0 ? random.below(size < 4096 ? size + 1 : 4096) : random.below(size + 1);
		ParseCopy(original.data(), cut, i % 2 == 0, path + " cut at " + std::to_string(cut), totals);
	}
	std::vector<uint8_t> changed;
	for (size_t i = 0; i < flips; i++) {
		changed = original;
		const uint64_t variant = random.next();
		Random variantRandom(variant);
		FlipBytes(changed, i % 2 == 0 ? 65536 : size, variantRandom);
		ParseCopy(changed.data(), changed.size(), i % 3 != 0, path + " changed, variant " + std::to_string(variant), totals);
	}
	for (size_t i = 0; i < flips / 4; i++) {
		changed = original;
		const uint64_t variant = random.next();
		Random variantRandom(variant);
		FlipBytes(changed, size, variantRandom);
		const size_t cut = random.below(size + 1);
		ParseCopy(changed.data(), cut, true, path + " changed, variant " + std::to_string(variant) + ", cut at " + std::to_string(cut), totals);
	}
}

void Time(const std::string &path, const std::vector<uint8_t> &bytes, bool movers) {
	double best = 1e300, bestNoEntities = 1e300;
	p2demo::Demo demo;
	std::string error;
	bool ok = false;
	for (int i = 0; i < 10; i++) {
		p2demo::ParseOptions full;
		full.movers = movers;
		Clock::time_point start = Clock::now();
		ok = p2demo::Parse(bytes.data(), bytes.size(), demo, error, full);
		best = std::min(best, MillisecondsSince(start));

		p2demo::Demo containerOnly;
		p2demo::ParseOptions options;
		options.entities = false;
		start = Clock::now();
		p2demo::Parse(bytes.data(), bytes.size(), containerOnly, error, options);
		bestNoEntities = std::min(bestNoEntities, MillisecondsSince(start));
	}
	size_t keys = 0;
	for (const auto &p : demo.players) keys += p.keys.size();
	for (const auto &p : demo.portals) keys += p.keys.size();
	for (const auto &m : demo.movers) keys += m.keys.size();
	std::printf("%8.2f ms  (container only %6.2f ms)  %9zu bytes  %6d ticks  %7zu keys  %s  %s\n", best, bestNoEntities, bytes.size(),
		demo.lastTick - demo.firstTick, keys, ok ? (demo.entitiesDecoded ? "ok" : "no entities") : "REJECTED", path.c_str());
}

// A number that changes if anything in the parsed demo does (enough to
// notice one thread's parse disturbing another's)
uint64_t Fingerprint(const p2demo::Demo &demo) {
	uint64_t h = 1469598103934665603ull;
	const auto add = [&h](const void *data, size_t size) {
		const unsigned char *bytes = static_cast<const unsigned char *>(data);
		for (size_t i = 0; i < size; i++) h = (h ^ bytes[i]) * 1099511628211ull;
	};
	const auto addInt = [&add](long long v) { add(&v, sizeof v); };
	const auto addVec = [&add](const p2demo::Vec3 &v) {
		add(&v.x, sizeof v.x);
		add(&v.y, sizeof v.y);
		add(&v.z, sizeof v.z);
	};
	addInt(demo.entitiesDecoded);
	addInt(demo.recorderEntIndex);
	for (const auto &v : demo.views) {
		addInt(v.tick);
		addVec(v.origin);
		addVec(v.angles);
	}
	for (const auto &c : demo.userCmds) {
		addInt(c.tick);
		addInt(c.buttons);
		addVec(c.viewAngles);
	}
	for (const auto &c : demo.consoleCmds) {
		addInt(c.tick);
		add(c.text.data(), c.text.size());
	}
	for (const auto &p : demo.players) {
		add(p.name.data(), p.name.size());
		for (const auto &k : p.keys) {
			addInt(k.tick);
			addVec(k.origin);
			addVec(k.velocity);
			addInt(k.flags);
			addInt(k.inSnapshot);
		}
	}
	for (const auto &p : demo.portals) {
		for (const auto &k : p.keys) {
			addInt(k.tick);
			addVec(k.origin);
			addVec(k.angles);
			addInt(k.active);
		}
	}
	for (const auto &m : demo.movers) {
		add(m.model.data(), m.model.size());
		for (const auto &k : m.keys) {
			addInt(k.tick);
			addVec(k.origin);
			addVec(k.angles);
			addInt(k.parent);
		}
	}
	for (const auto &shot : demo.portalShots) {
		addInt(shot.tick);
		addVec(shot.origin);
	}
	for (const auto &w : demo.warnings) add(w.data(), w.size());
	return h;
}

// false when a parse on a busy thread gives another result than alone
bool ParseOnThreads(const std::vector<std::vector<uint8_t>> &files, size_t threadCount) {
	std::vector<uint64_t> expected;
	for (const auto &bytes : files) {
		p2demo::Demo demo;
		std::string error;
		p2demo::Parse(bytes.data(), bytes.size(), demo, error);
		expected.push_back(Fingerprint(demo));
	}
	std::vector<size_t> mismatches(threadCount, 0);
	std::vector<std::thread> threads;
	const Clock::time_point start = Clock::now();
	for (size_t t = 0; t < threadCount; t++) {
		threads.emplace_back([&files, &expected, &mismatches, t] {
			for (int round = 0; round < 5; round++) {
				for (size_t i = 0; i < files.size(); i++) {
					// (each thread starts at another file, so different demos overlap)
					const size_t file = (i + t) % files.size();
					p2demo::Demo demo;
					std::string error;
					p2demo::Parse(files[file].data(), files[file].size(), demo, error);
					if (Fingerprint(demo) != expected[file]) mismatches[t]++;
				}
			}
		});
	}
	for (std::thread &thread : threads) thread.join();
	size_t total = 0;
	for (const size_t m : mismatches) total += m;
	std::printf("%zu threads x 5 rounds x %zu files in %.1f s: %zu results differ from the single-threaded parse\n", threadCount, files.size(),
		MillisecondsSince(start) / 1000, total);
	return total == 0;
}

}  // namespace

int main(int argc, char **argv) {
	size_t truncations = 50, flips = 300, threadCount = 0;
	uint64_t seed = 1;
	bool timeOnly = false;
	bool movers = true;
	bool usageError = false;
	std::vector<std::string> paths;
	for (int i = 1; i < argc; i++) {
		const std::string arg = argv[i];
		const bool hasValue = i + 1 < argc;
		if (arg == "--truncations" && hasValue) truncations = size_t(std::strtoull(argv[++i], nullptr, 10));
		else if (arg == "--flips" && hasValue) flips = size_t(std::strtoull(argv[++i], nullptr, 10));
		else if (arg == "--seed" && hasValue) seed = std::strtoull(argv[++i], nullptr, 10);
		else if (arg == "--time") timeOnly = true;
		else if (arg == "--no-movers") movers = false;
		else if (arg == "--threads" && hasValue) threadCount = size_t(std::strtoull(argv[++i], nullptr, 10));
		else if (!arg.empty() && arg[0] != '-') paths.push_back(arg);
		else usageError = true;
	}
	if (usageError || paths.empty()) {
		std::fprintf(stderr, "usage: demostress [--truncations N] [--flips N] [--seed S] <file.dem>...\n"
							 "       demostress --time [--no-movers] <file.dem>...\n"
							 "       demostress --threads N <file.dem>...\n");
		return 2;
	}
	if (threadCount > 0) {
		std::vector<std::vector<uint8_t>> files(paths.size());
		for (size_t i = 0; i < paths.size(); i++) {
			if (!ReadFile(paths[i], files[i])) {
				std::fprintf(stderr, "demostress: can't read %s\n", paths[i].c_str());
				return 1;
			}
		}
		return ParseOnThreads(files, threadCount > 64 ? 64 : threadCount) ? 0 : 1;
	}

	Random random(seed);
	Totals totals;
	bool failed = false;
	for (const std::string &path : paths) {
		std::vector<uint8_t> bytes;
		if (!ReadFile(path, bytes)) {
			std::fprintf(stderr, "demostress: can't read %s\n", path.c_str());
			failed = true;
			continue;
		}
		if (timeOnly) {
			Time(path, bytes, movers);
			continue;
		}
		const Totals before = totals;
		const Clock::time_point start = Clock::now();
		Stress(path, bytes, truncations, flips, random, totals);
		std::printf("%s: %zu variants in %.1f s, %zu accepted, %zu with entities decoded\n", path.c_str(), totals.parses - before.parses,
			MillisecondsSince(start) / 1000, totals.accepted - before.accepted, totals.entitiesDecoded - before.entitiesDecoded);
		std::fflush(stdout);
	}
	if (!timeOnly) {
		std::printf("total: %zu parses, no crash; %zu accepted, %zu with entities decoded; slowest %.0f ms (%s); %zu problems\n", totals.parses,
			totals.accepted, totals.entitiesDecoded, totals.slowestMs, totals.slowest.c_str(), totals.slow);
	}
	return failed || totals.slow ? 1 : 0;
}
