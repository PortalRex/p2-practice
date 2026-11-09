#include "ui/hud.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

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

}  // namespace

void Draw() {
	const trainer::Settings &settings = trainer::CurrentSettings();
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
