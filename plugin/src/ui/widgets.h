// The menu's controls, drawn and handled in one call each frame ("immediate mode"): a call draws the control
// where the layout puts it and says whether it was used this frame. State that outlives a frame (the value a
// toggle shows, the text in a box) belongs to the caller.
#pragma once

#include <string>
#include <vector>

#include "ui/gfx.h"

namespace ui {

using gfx::Color;
using gfx::Font;

// The colours (dark, like the game's own menus; blue and orange as the portals)
namespace theme {
constexpr Color kBackdrop{8, 10, 14, 150};
constexpr Color kWindow{22, 24, 30, 255};
constexpr Color kHeader{14, 15, 19, 255};
constexpr Color kField{36, 39, 47, 255};
constexpr Color kFieldHover{48, 52, 62, 255};
constexpr Color kLine{60, 64, 76, 255};
constexpr Color kText{236, 238, 242, 255};
constexpr Color kDim{150, 156, 168, 255};
constexpr Color kFaint{100, 105, 116, 255};
constexpr Color kBlue{40, 160, 255, 255};
constexpr Color kOrange{255, 150, 30, 255};
constexpr Color kZebra{255, 255, 255, 4};
constexpr Color kGreen{70, 200, 110, 255};
constexpr Color kRed{235, 80, 70, 255};
}  // namespace theme

struct Rect {
	int x = 0, y = 0, w = 0, h = 0;
	bool Contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
	Rect Inset(int d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
};

// Pixels at 1080p, scaled to the screen
int Px(float pixels);

// Starts a frame of the menu: reads the mouse. `enabled` false: everything is drawn but nothing reacts.
void BeginFrame(bool enabled = true);

// ---- layout: controls go one under the other inside the current area ----
void BeginArea(Rect area);
void EndArea();
Rect Take(int height);           // the next row of the current area, full width
Rect TakeWidth(int width, int height);  // the next piece of the current line (call SameLine before)
void SameLine();                 // the next control goes right of the last one
void Space(int pixels);
Rect Area();                     // what's left of the current area
int AreaWidth();

// ---- drawing helpers ----
void Panel(Rect r, Color fill, Color border);
// Text cut to `width` with "..." when longer. Returns the width drawn.
int TextIn(int x, int y, int width, Font font, Color color, const std::string &text);
int TextRight(int right, int y, Font font, Color color, const std::string &text);
int TextCentered(Rect r, Font font, Color color, const std::string &text);
std::string Fit(Font font, const std::string &text, int width);

// ---- named spots: where controls were drawn last frame, by name (their label), for the scripted tour ----
void Spot(const std::string &name, Rect r);
bool FindSpot(const std::string &name, Rect &r);

// ---- controls ----
bool Hovered(Rect r);
bool Clicked(Rect r);             // pressed with the left button inside `r` this frame
void Label(const std::string &text, Font font = Font::Normal, Color color = theme::kText);
void Caption(const std::string &text);  // small grey heading over a group of controls
bool Button(const std::string &text, bool primary = false, bool enabled = true);
bool ButtonIn(Rect r, const std::string &text, bool primary = false, bool enabled = true);
bool Toggle(const std::string &text, bool &value);
// Segmented buttons: one of `options`. True when changed.
bool Choice(const std::string &caption, int &value, const std::vector<std::string> &options, const std::vector<Color> &colors = {});
// "- value +" with a step, kept within [lo, hi]
bool Stepper(const std::string &caption, float &value, float step, float lo, float hi, const char *format);
bool Slider(const std::string &caption, float &value, float lo, float hi, const char *format);
// One line of text; true when Enter was pressed in it
bool TextBox(const char *id, std::string &text, const std::string &placeholder);
bool AnyTextFocused();
void Unfocus();

// A list of `count` rows of `rowHeight` pixels inside `r`, scrolled by the wheel and its bar. Draw the rows
// between `first` and `last` (exclusive) with ListRow.
struct List {
	Rect area;
	int rowHeight = 0, count = 0, first = 0, last = 0;
	int *scroll = nullptr;
};
List BeginList(Rect r, int rowHeight, int count, int &scroll);
// The rectangle of row `index`; `clicked` is set when it was clicked
Rect ListRow(const List &list, int index, bool selected, bool &clicked);
void EndList(const List &list);
// Scrolls a list so that `index` is in view
void ScrollTo(int &scroll, int index, int rowHeight, int height, int count);

}  // namespace ui
