#include "ui/widgets.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>

namespace ui {
namespace {

struct Layout {
	Rect area;
	int y = 0;          // where the next row starts
	Rect last;          // the last control placed
	bool sameLine = false;
};

std::vector<Layout> g_layouts;
std::map<std::string, Rect> g_spots, g_lastSpots;
bool g_enabled = true;
bool g_mousePressed = false, g_mouseDown = false;
int g_mx = 0, g_my = 0;
size_t g_active = 0;        // the control being dragged (slider, scroll bar), by id
std::string g_focus;        // the text box being typed into
int g_repeatKey = -1;       // a key held down in a text box repeats
DWORD g_repeatAt = 0;

size_t Id(const std::string &text, int salt = 0) { return std::hash<std::string>{}(text) ^ static_cast<size_t>(salt) * 0x9E3779B9u; }

int Gap() { return Px(8); }
int RowHeight() { return Px(36); }

Layout &Current() {
	static Layout screen;
	if (g_layouts.empty()) {
		screen.area = {0, 0, gfx::ScreenWidth(), gfx::ScreenHeight()};
		return screen;
	}
	return g_layouts.back();
}

int CenterY(Rect r, Font font) { return r.y + (r.h - gfx::FontHeight(font)) / 2; }

std::string Clipboard() {
	std::string out;
	if (!OpenClipboard(nullptr)) return out;
	if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
		if (auto *wide = static_cast<const wchar_t *>(GlobalLock(data))) {
			int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
			if (size > 1) {
				out.resize(size);
				WideCharToMultiByte(CP_UTF8, 0, wide, -1, &out[0], size, nullptr, nullptr);
				out.resize(size - 1);
			}
			GlobalUnlock(data);
		}
	}
	CloseClipboard();
	out.erase(std::remove_if(out.begin(), out.end(), [](char c) { return c == '\r' || c == '\n'; }), out.end());
	return out;
}

// A key held in a text box acts once, then again and again after a moment
bool Repeats(int key) {
	if (gfx::KeyPressed(key)) {
		g_repeatKey = key;
		g_repeatAt = GetTickCount() + 400;
		return true;
	}
	if (g_repeatKey == key && gfx::KeyDown(key) && GetTickCount() >= g_repeatAt) {
		g_repeatAt = GetTickCount() + 35;
		return true;
	}
	return false;
}

}  // namespace

int Px(float pixels) { return static_cast<int>(std::lround(pixels * gfx::Scale())); }

void BeginFrame(bool enabled) {
	g_enabled = enabled;
	g_layouts.clear();
	g_lastSpots.swap(g_spots);
	g_spots.clear();
	g_mx = gfx::MouseX();
	g_my = gfx::MouseY();
	g_mouseDown = enabled && gfx::KeyDown(gfx::kMouseLeft);
	g_mousePressed = enabled && gfx::KeyPressed(gfx::kMouseLeft);
	if (!g_mouseDown) g_active = 0;
	// a click anywhere else takes the focus off a text box (the box itself takes it back below)
	if (g_mousePressed) g_focus.clear();
}

void BeginArea(Rect area) {
	Layout layout;
	layout.area = area;
	layout.y = area.y;
	layout.last = {area.x, area.y, 0, 0};
	g_layouts.push_back(layout);
}

void EndArea() {
	if (!g_layouts.empty()) g_layouts.pop_back();
}

Rect Take(int height) {
	Layout &l = Current();
	if (l.sameLine) {
		l.sameLine = false;
		int x = l.last.x + l.last.w + Gap();
		Rect r{x, l.last.y, l.area.x + l.area.w - x, height};
		l.last = r;
		l.y = std::max(l.y, r.y + height + Gap());
		return r;
	}
	Rect r{l.area.x, l.y, l.area.w, height};
	l.y += height + Gap();
	l.last = r;
	return r;
}

Rect TakeWidth(int width, int height) {
	Layout &l = Current();
	Rect r = Take(height);
	r.w = std::min(width, r.w);
	l.last = r;
	return r;
}

void SameLine() { Current().sameLine = true; }
void Space(int pixels) { Current().y += pixels; }
Rect Area() {
	Layout &l = Current();
	return {l.area.x, l.y, l.area.w, l.area.y + l.area.h - l.y};
}
int AreaWidth() { return Current().area.w; }

void Panel(Rect r, Color fill, Color border) {
	gfx::FillRect(r.x, r.y, r.w, r.h, fill);
	if (border.a) gfx::OutlineRect(r.x, r.y, r.w, r.h, border);
}

std::string Fit(Font font, const std::string &text, int width) {
	if (width <= 0) return "";
	if (gfx::TextWidth(font, text.c_str()) <= width) return text;
	std::string cut = text;
	while (!cut.empty()) {
		// (drop whole UTF-8 characters)
		do cut.pop_back();
		while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80);
		if (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0x80)) cut.pop_back();
		if (gfx::TextWidth(font, (cut + "...").c_str()) <= width) return cut + "...";
	}
	return "";
}

int TextIn(int x, int y, int width, Font font, Color color, const std::string &text) {
	return gfx::Text(x, y, font, color, Fit(font, text, width).c_str());
}

int TextRight(int right, int y, Font font, Color color, const std::string &text) {
	int w = gfx::TextWidth(font, text.c_str());
	gfx::Text(right - w, y, font, color, text.c_str());
	return w;
}

int TextCentered(Rect r, Font font, Color color, const std::string &text) {
	std::string shown = Fit(font, text, r.w);
	int w = gfx::TextWidth(font, shown.c_str());
	gfx::Text(r.x + (r.w - w) / 2, CenterY(r, font), font, color, shown.c_str());
	return w;
}

void Spot(const std::string &name, Rect r) { g_spots[name] = r; }

bool FindSpot(const std::string &name, Rect &r) {
	auto it = g_lastSpots.find(name);
	if (it == g_lastSpots.end()) return false;
	r = it->second;
	return true;
}

bool Hovered(Rect r) { return g_enabled && r.Contains(g_mx, g_my); }
bool Clicked(Rect r) { return g_mousePressed && r.Contains(g_mx, g_my); }

void Label(const std::string &text, Font font, Color color) {
	Rect r = Take(gfx::FontHeight(font));
	TextIn(r.x, r.y, r.w, font, color, text);
}

void Caption(const std::string &text) {
	Space(Px(4));
	Label(text, Font::Small, theme::kFaint);
}

bool ButtonIn(Rect r, const std::string &text, bool primary, bool enabled) {
	Spot(text, r);
	bool hover = enabled && Hovered(r);
	Color fill = primary ? (hover ? Color(70, 180, 255) : theme::kBlue) : (hover ? theme::kFieldHover : theme::kField);
	if (!enabled) fill = theme::kField.Alpha(120);
	Panel(r, fill, primary ? Color(0, 0, 0, 0) : theme::kLine);
	TextCentered(r, primary ? Font::Bold : Font::Normal, enabled ? (primary ? Color(10, 14, 20) : theme::kText) : theme::kFaint, text);
	return enabled && Clicked(r);
}

bool Button(const std::string &text, bool primary, bool enabled) { return ButtonIn(Take(RowHeight()), text, primary, enabled); }

bool Toggle(const std::string &text, bool &value) {
	Rect r = Take(RowHeight());
	Spot(text, r);
	bool hover = Hovered(r);
	int box = Px(22);
	Rect b{r.x, r.y + (r.h - box) / 2, box, box};
	Panel(b, value ? theme::kBlue : (hover ? theme::kFieldHover : theme::kField), value ? Color(0, 0, 0, 0) : theme::kLine);
	if (value) {
		// a tick
		int x = b.x, y = b.y, s = box;
		for (int t = 0; t < 2; t++) {
			gfx::Line(x + s * 22 / 100, y + s * 52 / 100 + t, x + s * 42 / 100, y + s * 72 / 100 + t, Color(10, 14, 20));
			gfx::Line(x + s * 42 / 100, y + s * 72 / 100 + t, x + s * 78 / 100, y + s * 30 / 100 + t, Color(10, 14, 20));
		}
	}
	TextIn(b.x + box + Px(12), CenterY(r, Font::Normal), r.w - box - Px(12), Font::Normal, hover ? theme::kText : Color(215, 218, 224), text);
	if (Clicked(r)) {
		value = !value;
		return true;
	}
	return false;
}

namespace {
// A row with a caption in a left column; returns the rest
Rect CaptionColumn(const std::string &caption, Rect r) {
	if (caption.empty()) return r;
	int column = std::min(Px(150), r.w / 3);
	TextIn(r.x, CenterY(r, Font::Small), column - Px(8), Font::Small, theme::kDim, caption);
	return {r.x + column, r.y, r.w - column, r.h};
}
}  // namespace

bool Choice(const std::string &caption, int &value, const std::vector<std::string> &options, const std::vector<Color> &colors) {
	Rect r = CaptionColumn(caption, Take(RowHeight()));
	bool changed = false;
	int n = static_cast<int>(options.size());
	for (int i = 0; i < n; i++) {
		int x0 = r.x + r.w * i / n, x1 = r.x + r.w * (i + 1) / n;
		Rect s{x0, r.y, x1 - x0 - (i + 1 < n ? Px(4) : 0), r.h};
		Spot(options[i], s);
		bool on = value == i;
		Color accent = i < static_cast<int>(colors.size()) ? colors[i] : theme::kBlue;
		bool hover = Hovered(s);
		Panel(s, on ? accent.Alpha(60) : (hover ? theme::kFieldHover : theme::kField), on ? accent : theme::kLine);
		if (on) gfx::FillRect(s.x, s.y + s.h - Px(3), s.w, Px(3), accent);
		TextCentered(s, on ? Font::Bold : Font::Normal, on ? theme::kText : theme::kDim, options[i]);
		if (Clicked(s) && !on) {
			value = i;
			changed = true;
		}
	}
	return changed;
}

bool Stepper(const std::string &caption, float &value, float step, float lo, float hi, const char *format) {
	Rect r = CaptionColumn(caption, Take(RowHeight()));
	int b = r.h;
	Rect minus{r.x, r.y, b, b}, plus{r.x + r.w - b, r.y, b, b}, field{r.x + b + Px(4), r.y, r.w - 2 * b - Px(8), b};
	Panel(field, theme::kField, theme::kLine);
	char text[32];
	snprintf(text, sizeof(text), format, value);
	TextCentered(field, Font::Bold, theme::kText, text);
	float before = value;
	if (ButtonIn(minus, "-")) value -= step;
	if (ButtonIn(plus, "+")) value += step;
	if (Hovered(field) && gfx::WheelSteps()) value += step * gfx::WheelSteps();
	value = std::round(std::clamp(value, lo, hi) / step) * step;
	return value != before;
}

bool Slider(const std::string &caption, float &value, float lo, float hi, const char *format) {
	Rect r = CaptionColumn(caption, Take(RowHeight()));
	size_t id = Id(caption, 1);
	char text[32];
	snprintf(text, sizeof(text), format, value);
	int textWidth = Px(70);
	Rect bar{r.x, r.y + r.h / 2 - Px(3), r.w - textWidth - Px(10), Px(6)};
	Rect grab{bar.x - Px(6), r.y, bar.w + Px(12), r.h};
	Spot(caption, bar);
	if (Clicked(grab)) g_active = id;
	float before = value;
	if (g_active == id && g_mouseDown && bar.w > 0) value = lo + (hi - lo) * std::clamp(static_cast<float>(g_mx - bar.x) / bar.w, 0.0f, 1.0f);
	float t = hi > lo ? std::clamp((value - lo) / (hi - lo), 0.0f, 1.0f) : 0;
	Panel(bar, theme::kField, Color(0, 0, 0, 0));
	gfx::FillRect(bar.x, bar.y, static_cast<int>(bar.w * t), bar.h, theme::kBlue);
	int knob = Px(16);
	gfx::FillRect(bar.x + static_cast<int>(bar.w * t) - knob / 2, r.y + (r.h - knob) / 2, knob, knob, Hovered(grab) || g_active == id ? Color(255, 255, 255) : Color(215, 220, 228));
	TextRight(r.x + r.w, CenterY(r, Font::Normal), Font::Normal, theme::kText, text);
	return value != before;
}

bool TextBox(const char *id, std::string &text, const std::string &placeholder) {
	Rect r = Take(RowHeight());
	Spot(id, r);
	bool focused = g_focus == id;
	if (Clicked(r)) {
		g_focus = id;
		focused = true;
	}
	Panel(r, focused ? theme::kFieldHover : theme::kField, focused ? theme::kBlue : theme::kLine);
	bool entered = false;
	if (focused && g_enabled) {
		bool control = gfx::KeyDown(gfx::kKeyLControl) || gfx::KeyDown(gfx::kKeyRControl);
		if (control && gfx::KeyPressed(gfx::kKeyA + ('v' - 'a'))) text += Clipboard();
		else if (!control) text += gfx::TypedText();
		if (Repeats(gfx::kKeyBackspace) && !text.empty()) {
			if (control) text.clear();
			else {
				do text.pop_back();
				while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80);
				if (!text.empty() && (static_cast<unsigned char>(text.back()) & 0x80)) text.pop_back();
			}
		}
		if (gfx::KeyPressed(gfx::kKeyEnter) || gfx::KeyPressed(gfx::kKeyPadEnter)) entered = true;
		if (gfx::KeyPressed(gfx::kKeyEscape)) g_focus.clear();
	}
	int pad = Px(10);
	int y = CenterY(r, Font::Normal);
	if (text.empty() && !focused) TextIn(r.x + pad, y, r.w - 2 * pad, Font::Normal, theme::kFaint, placeholder);
	else {
		// the end of the text stays in view
		std::string shown = text;
		while (!shown.empty() && gfx::TextWidth(Font::Normal, shown.c_str()) > r.w - 2 * pad - Px(4)) shown.erase(0, 1);
		int w = gfx::Text(r.x + pad, y, Font::Normal, theme::kText, shown.c_str());
		if (focused && (GetTickCount() / 500) % 2 == 0) gfx::FillRect(r.x + pad + w + Px(1), r.y + Px(7), Px(2), r.h - Px(14), theme::kText);
	}
	return entered;
}

bool AnyTextFocused() { return !g_focus.empty(); }
void Unfocus() { g_focus.clear(); }

List BeginList(Rect r, int rowHeight, int count, int &scroll) {
	List list;
	list.area = r;
	list.rowHeight = std::max(1, rowHeight);
	list.count = count;
	list.scroll = &scroll;
	int content = count * list.rowHeight;
	int maxScroll = std::max(0, content - r.h);
	if (Hovered(r) && gfx::WheelSteps()) scroll -= gfx::WheelSteps() * list.rowHeight * 3;
	// the scroll bar, dragged
	size_t id = Id("list", r.x * 7919 + r.y);
	Rect track{r.x + r.w - Px(8), r.y, Px(8), r.h};
	if (maxScroll > 0 && Clicked(track)) g_active = id;
	if (g_active == id && g_mouseDown && maxScroll > 0)
		scroll = static_cast<int>(std::clamp(static_cast<float>(g_my - r.y) / r.h, 0.0f, 1.0f) * content - r.h / 2.0f);
	scroll = std::clamp(scroll, 0, maxScroll);
	list.first = std::min(count, scroll / list.rowHeight);
	list.last = std::min(count, (scroll + r.h + list.rowHeight - 1) / list.rowHeight);
	return list;
}

Rect ListRow(const List &list, int index, bool selected, bool &clicked) {
	Rect row{list.area.x, list.area.y + index * list.rowHeight - *list.scroll, list.area.w - Px(12), list.rowHeight};
	clicked = false;
	// rows cut by the list's edges aren't drawn (no clipping): only whole ones
	if (row.y < list.area.y || row.y + row.h > list.area.y + list.area.h) return {row.x, -10000, row.w, row.h};
	bool hover = Hovered(row);
	if (selected) {
		gfx::FillRect(row.x, row.y, row.w, row.h, theme::kBlue.Alpha(55));
		gfx::FillRect(row.x, row.y, Px(3), row.h, theme::kBlue);
	} else if (hover) gfx::FillRect(row.x, row.y, row.w, row.h, theme::kFieldHover);
	else if (index % 2) gfx::FillRect(row.x, row.y, row.w, row.h, theme::kZebra);
	clicked = Clicked(row);
	return row;
}

void EndList(const List &list) {
	Rect r = list.area;
	int content = list.count * list.rowHeight;
	if (content <= r.h) return;
	Rect track{r.x + r.w - Px(6), r.y, Px(6), r.h};
	gfx::FillRect(track.x, track.y, track.w, track.h, theme::kField);
	int thumb = std::max(Px(30), r.h * r.h / content);
	int y = r.y + static_cast<int>(static_cast<long long>(r.h - thumb) * *list.scroll / std::max(1, content - r.h));
	gfx::FillRect(track.x, y, track.w, thumb, theme::kDim);
}

void ScrollTo(int &scroll, int index, int rowHeight, int height, int count) {
	int top = index * rowHeight;
	if (top < scroll) scroll = top;
	else if (top + rowHeight > scroll + height) scroll = top + rowHeight - height;
	scroll = std::clamp(scroll, 0, std::max(0, count * rowHeight - height));
}

}  // namespace ui
