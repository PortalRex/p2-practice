#include "ui/hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "bhop.h"
#include "trainer.h"
#include "ui/widgets.h"

namespace hud {
namespace {

using gfx::Color;
using gfx::Font;

// Text with a soft dark edge, readable on any wall
int Shadowed(int x, int y, Font font, Color color, const std::string &text) {
	int d = std::max(1, ui::Px(2));
	gfx::Text(x + d, y + d, font, Color(0, 0, 0, color.a * 3 / 5), text.c_str());
	return gfx::Text(x, y, font, color, text.c_str());
}

int Centered(int y, Font font, Color color, const std::string &text) {
	int w = gfx::TextWidth(font, text.c_str());
	Shadowed((gfx::ScreenWidth() - w) / 2, y, font, color, text);
	return w;
}

// The bhop trainer, bottom left above the speedometer: the last jump (gain, share of air time gaining, hop timing),
// the chain of hops, and the latest jumps' gains as bars
void DrawBhop() {
	const bhop::Status &b = bhop::Current();
	const Color white(255, 255, 255), grey(185, 190, 200), faint(120, 125, 135);
	int x = ui::Px(32), y = gfx::ScreenHeight() - ui::Px(300);
	char text[96];
	int w = Shadowed(x, y, Font::Small, grey, "BHOP");
	if (b.chain > 1) {
		snprintf(text, sizeof(text), "chain %d", b.chain);
		Shadowed(x + w + ui::Px(10), y, Font::Small, white, text);
	}
	y += gfx::FontHeight(Font::Small);
	// the jump in the air now, else the last one
	const bhop::Jump *jump = b.inAir ? &b.current : b.recent.empty() ? nullptr : &b.recent.back();
	if (!jump) {
		Shadowed(x, y, Font::Normal, faint, "jump to start");
		return;
	}
	if (jump->thrown) {
		Shadowed(x, y, Font::Normal, faint, "thrown (portal, plate or gel): not counted");
	} else {
		float gain = b.inAir ? 0 : jump->Gain();
		int gw = 0;
		if (!b.inAir) {
			snprintf(text, sizeof(text), "%+.0f", gain);
			gw = Shadowed(x, y, Font::Title, gain >= 0 ? ui::theme::kGreen : ui::theme::kOrange, text) + ui::Px(12);
		}
		int ty = y + (gfx::FontHeight(Font::Title) - gfx::FontHeight(Font::Normal)) / 2;
		int percent = jump->GainingPercent();
		snprintf(text, sizeof(text), "%d%% gaining", percent);
		gw += Shadowed(x + gw, ty, Font::Normal, percent >= 80 ? ui::theme::kGreen : percent >= 60 ? white : ui::theme::kOrange, text) + ui::Px(12);
		// (the landing tick counts as one: jumping on the next is as quick as a hop gets. A jump after walking a
		// while is no hop at all: nothing to say about its timing.)
		constexpr int kHopAttempt = 10;
		if (jump->groundTicks >= 0 && jump->groundTicks <= kHopAttempt) {
			bool perfect = jump->groundTicks <= 1;
			if (perfect) snprintf(text, sizeof(text), "hop: perfect");
			else snprintf(text, sizeof(text), "hop: %d ticks late", jump->groundTicks - 1);
			Shadowed(x + gw, ty, Font::Normal, perfect ? ui::theme::kGreen : ui::theme::kOrange, text);
		}
	}
	y += gfx::FontHeight(Font::Title) + ui::Px(4);
	// the latest jumps' gains, as bars around a middle line (thrown ones grey)
	constexpr int kBars = 12;  // (as many as bhop keeps)
	int barW = ui::Px(10), gap = ui::Px(4), half = ui::Px(22);
	gfx::FillRect(x, y + half, kBars * (barW + gap), 1, Color(255, 255, 255, 70));
	for (size_t i = 0; i < b.recent.size(); i++) {
		const bhop::Jump &j = b.recent[i];
		float g = j.thrown ? 0 : j.Gain();
		int h = std::clamp(static_cast<int>(std::fabs(g) / 40.0f * half), j.thrown ? 0 : 2, half);
		Color c = j.thrown ? faint : g >= 0 ? ui::theme::kGreen : ui::theme::kOrange;
		int bx = x + static_cast<int>(i) * (barW + gap);
		if (g >= 0) gfx::FillRect(bx, y + half - h, barW, h, c);
		else gfx::FillRect(bx, y + half, barW, h, c);
	}
}

}  // namespace

void Draw() {
	const trainer::Settings &settings = trainer::CurrentSettings();
	if (settings.strafe && gfx::InGame()) DrawBhop();
	if (!settings.hud) return;
	trainer::Status s = trainer::CurrentStatus();
	if (!s.loaded || (!s.active && s.flash.empty())) return;
	const Color white(255, 255, 255), grey(185, 190, 200);

	int y = ui::Px(38);
	if (!s.clock.empty()) {
		Centered(y, Font::Huge, white, s.clock);
		y += gfx::FontHeight(Font::Huge) - ui::Px(6);
	}
	if (s.gapShown) {
		Color color = s.ahead ? ui::theme::kBlue : ui::theme::kOrange;
		Centered(y, Font::Title, color, s.gap + (s.finished ? "  FINISH" : ""));
		y += gfx::FontHeight(Font::Title);
	} else if (!s.note.empty()) {
		Centered(y, Font::Normal, grey, s.note);
		y += gfx::FontHeight(Font::Normal);
	}

	if (!s.flash.empty()) {
		// a band behind the message
		int w = gfx::TextWidth(Font::Title, s.flash.c_str()) + ui::Px(48), h = gfx::FontHeight(Font::Title) + ui::Px(16);
		int fy = y + ui::Px(26);
		gfx::FillRect((gfx::ScreenWidth() - w) / 2, fy, w, h, Color(10, 12, 16, 150));
		Centered(fy + ui::Px(8), Font::Title, white, s.flash);
	}

	// the part, top left
	if (s.part >= 0 && s.part < static_cast<int>(s.parts.size())) {
		char text[160];
		snprintf(text, sizeof(text), "PART %d/%d", s.part + 1, static_cast<int>(s.parts.size()));
		int x = ui::Px(32), py = ui::Px(30);
		Shadowed(x, py, Font::Small, grey, text);
		Shadowed(x, py + gfx::FontHeight(Font::Small), Font::Bold, white, s.parts[s.part].name);
		if (s.attempts > 0) {
			snprintf(text, sizeof(text), "attempt %d", s.attempts);
			int ay = py + gfx::FontHeight(Font::Small) + gfx::FontHeight(Font::Bold);
			int w = Shadowed(x, ay, Font::Small, grey, text);
			// the best finish of the session from this part, against the record (what this attempt is up against)
			const trainer::Result *best = nullptr;
			for (const trainer::Result &r : s.results)
				if (r.part == s.part && (!best || r.gap < best->gap)) best = &r;
			if (best) {
				int hundredths = static_cast<int>(best->gap * 100 + (best->gap < 0 ? -0.5f : 0.5f));
				snprintf(text, sizeof(text), "best %c%d.%02d", hundredths < 0 ? '-' : '+', std::abs(hundredths) / 100, std::abs(hundredths) % 100);
				Shadowed(x + w + ui::Px(14), ay, Font::Small, best->gap <= 0 ? ui::theme::kGreen : ui::theme::kOrange, text);
			}
		}
	}

	// the speed, bottom left
	if (settings.speedometer && s.speed >= 0) {
		char speed[32], record[48];
		snprintf(speed, sizeof(speed), "%.0f", s.speed);
		snprintf(record, sizeof(record), "RECORD %.0f", s.recordSpeed);
		int x = ui::Px(32), by = gfx::ScreenHeight() - ui::Px(110);
		Shadowed(x, by, Font::Small, grey, "SPEED");
		int w = Shadowed(x, by + gfx::FontHeight(Font::Small), Font::Title, white, speed);
		Color compare = s.speed >= s.recordSpeed ? ui::theme::kBlue : ui::theme::kOrange;
		Shadowed(x + w + ui::Px(14), by + gfx::FontHeight(Font::Small) + ui::Px(8), Font::Normal, compare, record);
	}
}

}  // namespace hud
