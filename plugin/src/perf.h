// Development: where the plugin's time goes ("prac_debug perf" prints it and starts counting again)
#pragma once

#include <windows.h>

namespace perf {

enum Section { kGameFrame, kScript, kPaint, kSections };

struct Counters {
	long long ticks[kSections] = {};
	int calls[kSections] = {};
	int frames = 0;  // (frames drawn: one per pass of the 2D panels)
	long long since = 0;
};

inline Counters g_counters;

inline long long Now() {
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return now.QuadPart;
}

class Scope {
public:
	explicit Scope(Section section) : section_(section), start_(Now()) {}
	~Scope() {
		g_counters.ticks[section_] += Now() - start_;
		g_counters.calls[section_]++;
	}

private:
	Section section_;
	long long start_;
};

}  // namespace perf
