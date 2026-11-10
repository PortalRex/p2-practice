#include "ui/gfx.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "engine.h"
#include "perf.h"

namespace gfx {
namespace {

// ---- the game's interfaces and their function-table slots (Windows; the same in SAR's and the 2013 SDK's
// layouts, checked against the game's binaries) ----
namespace slot {
// ISurface (VGUI_Surface031)
constexpr int DrawSetColor = 14;        // (int r, int g, int b, int a)
constexpr int DrawFilledRect = 15;      // (x0, y0, x1, y1)
constexpr int DrawOutlinedRect = 17;
constexpr int DrawLine = 18;
constexpr int GetScreenSize = 42;       // (int &wide, int &tall)
constexpr int UnlockCursor = 64;
constexpr int LockCursor = 65;
constexpr int CreateFont = 69;          // () -> HFont
constexpr int SetFontGlyphSet = 70;     // (font, name, tall, weight, blur, scanlines, flags, rangeMin, rangeMax)
constexpr int GetFontTall = 72;
constexpr int GetTextSize = 77;         // (font, const wchar_t *, int &wide, int &tall)
constexpr int PaintTraverseEx = 117;    // calls the surface's StartDrawing and FinishDrawing (found from it)
constexpr int DrawFilledRectFade = 121; // (x0, y0, x1, y1, alpha0, alpha1, horizontal)
constexpr int DrawColoredText = 160;    // __cdecl (this, font, x, y, r, g, b, a, format, ...)
constexpr int HandleInputEvent = 166;   // (const InputEvent_t &) -> bool: hands the event to VGUI's panels
// The relative call operands of StartDrawing and FinishDrawing inside PaintTraverseEx
constexpr int StartDrawingCall = 0x7f;
constexpr int FinishDrawingCall = 0x25b;
// IInputSystem (InputSystemVersion001)
constexpr int IsButtonDown = 14;
constexpr int GetButtonPressedTick = 15;
constexpr int GetCursorPosition = 45;
// IInputStackSystem (InputStackSystemVersion001)
constexpr int SetCursorVisible = 11;    // (context, bool)
// IEngineVGui (VEngineVGui001)
constexpr int Paint = 14;               // (PaintMode_t)
// IVEngineClient (VEngineClient015)
constexpr int ConIsVisible = 11;
constexpr int GetInputContext = 190;    // (int id) -> context of that split-screen player's input
// IBaseClientDLL (VClient016)
constexpr int InKeyEvent = 20;          // (int down, ButtonCode_t, const char *binding) -> int: 0 = swallowed
// vgui::IInput (VGUI_Input005)
constexpr int UpdateButtonState = 87;   // (const InputEvent_t *)
constexpr int ResetInputContext = 88;   // (HInputContext)
constexpr int InputContextField = 0xce8;  // the input's current context handle (m_hContext)
}  // namespace slot

constexpr int kPaintUiPanels = 1;  // PaintMode_t: the 2D panels pass (HUD, menus)
constexpr int kFontAntialias = 0x010, kFontDropShadow = 0x080, kFontOutline = 0x200;

template <typename R, typename... Args> R Call(void *object, int index, Args... args) {
	using Fn = R(__thiscall *)(void *, Args...);
	return reinterpret_cast<Fn>((*static_cast<void ***>(object))[index])(object, args...);
}

void *GetInterface(const char *module, const char *name) {
	HMODULE handle = GetModuleHandleA(module);
	if (!handle) return nullptr;
	using Factory = void *(__cdecl *)(const char *, int *);
	auto factory = reinterpret_cast<Factory>(GetProcAddress(handle, "CreateInterface"));
	return factory ? factory(name, nullptr) : nullptr;
}

void *g_surface, *g_input, *g_inputStack, *g_engineVgui, *g_engineClient, *g_client, *g_vguiInput;
using StartDrawingFn = void(__thiscall *)(void *);
using FinishDrawingFn = void(__cdecl *)();
StartDrawingFn g_startDrawing;
FinishDrawingFn g_finishDrawing;
bool g_ready = false;
void (*g_draw)() = nullptr;

// ---- one patched function-table slot ----
struct Patch {
	void **slot = nullptr;      // where the hook is installed now (null: nowhere)
	void *original = nullptr;   // what that slot held before
	void *first = nullptr;      // what the slot held the first time the hook went in: the game's own function
	void *hook = nullptr;
	bool inside = false;        // a call is passing through the hook right now
};

bool Install(Patch &patch, void *object, int index, void *hook) {
	void **table = *static_cast<void ***>(object);
	DWORD old;
	if (!VirtualProtect(&table[index], sizeof(void *), PAGE_READWRITE, &old)) return false;
	patch.slot = &table[index];
	patch.original = table[index];
	if (!patch.first) patch.first = patch.original;
	patch.hook = hook;
	table[index] = hook;
	VirtualProtect(&table[index], sizeof(void *), old, &old);
	return true;
}

void Remove(Patch &patch) {
	if (!patch.slot) return;
	DWORD old;
	// (only where the slot still holds this hook: another plugin may have put its own over it since)
	if (*patch.slot == patch.hook && VirtualProtect(patch.slot, sizeof(void *), PAGE_READWRITE, &old)) {
		*patch.slot = patch.original;
		VirtualProtect(patch.slot, sizeof(void *), old, &old);
	}
	// (`first` stays: a copy of the table made by another plugin may still call the hook, which passes on)
	patch.slot = nullptr;
}

// One call on its way through a hook. Another plugin (SAR) copies the function table with this hook in it and
// puts its own hook in front, so the hook stays in that plugin's chain when this one is unloaded; loaded again,
// it goes in front of that plugin's and is then called twice per call: first from the table (`own`: the call to
// act on, handed on to what the table held), then by the other plugin, which is handed on to the game's own
// function. Without the second case the two hooks would call each other for ever.
struct Passing {
	explicit Passing(Patch &patch) : patch_(patch), own(patch.slot && !patch.inside), next(own ? patch.original : patch.first) {
		if (own) patch_.inside = true;
	}
	~Passing() {
		if (own) patch_.inside = false;
	}
	Passing(const Passing &) = delete;
	Passing &operator=(const Passing &) = delete;

private:
	Patch &patch_;

public:
	const bool own;
	void *const next;
};

Patch g_paint, g_lockCursor, g_keyEvent, g_buttonState, g_inputEvent;

// ---- input state ----
bool g_down[kButtonCount], g_wasDown[kButtonCount];
int g_wheelUpTick = -1, g_wheelDownTick = -1, g_wheel = 0;
int g_mouseX = 0, g_mouseY = 0;
std::string g_typed;
bool g_capture = false, g_cursorShown = false;
bool g_puppet = false, g_puppetLeft = false;
int g_puppetX = 0, g_puppetY = 0, g_puppetWheel = 0;
std::string g_puppetTyped;

// ---- fonts ----
unsigned long g_fonts[static_cast<int>(Font::Count)];  // (0: not made yet)
int g_fontHeight[static_cast<int>(Font::Count)];
int g_fontScreenHeight = 0;
int g_width = 0, g_height = 0;

std::wstring Wide(const char *text) {
	int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
	std::wstring out(size > 0 ? size : 1, L'\0');
	if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text, -1, &out[0], size);
	out.resize(wcslen(out.c_str()));
	return out;
}

void DropReleasedStrafeKeys();

void PollInput() {
	DropReleasedStrafeKeys();
	memcpy(g_wasDown, g_down, sizeof(g_down));
	for (int key = 0; key < kButtonCount; key++) g_down[key] = Call<bool, int>(g_input, slot::IsButtonDown, key);
	// (the wheel is a button pressed and released at once: each step moves its "last pressed" tick)
	g_wheel = 0;
	int up = Call<int, int>(g_input, slot::GetButtonPressedTick, kMouseWheelUp);
	int down = Call<int, int>(g_input, slot::GetButtonPressedTick, kMouseWheelDown);
	if (g_wheelUpTick >= 0 && up != g_wheelUpTick) g_wheel++;
	if (g_wheelDownTick >= 0 && down != g_wheelDownTick) g_wheel--;
	g_wheelUpTick = up;
	g_wheelDownTick = down;
	Call<void, int *, int *>(g_input, slot::GetCursorPosition, &g_mouseX, &g_mouseY);

	g_typed.clear();
	bool shift = g_down[kKeyLShift] || g_down[kKeyRShift];
	for (int key = 0; key < kButtonCount; key++) {
		if (!g_down[key] || g_wasDown[key]) continue;
		if (key >= kKeyA && key < kKeyA + 26) g_typed += static_cast<char>((shift ? 'A' : 'a') + key - kKeyA);
		else if (key >= kKey0 && key < kKey0 + 10) g_typed += shift ? ")!@#$%^&*("[key - kKey0] : static_cast<char>('0' + key - kKey0);
		else if (key >= kKeyPad0 && key < kKeyPad0 + 10) g_typed += static_cast<char>('0' + key - kKeyPad0);
		else if (key == kKeyPadDecimal) g_typed += '.';
		else if (key == kKeySpace) g_typed += ' ';
		else if (key == kKeyMinus) g_typed += shift ? '_' : '-';
		else if (key == kKeyPeriod) g_typed += shift ? '>' : '.';
		else if (key == kKeyComma) g_typed += shift ? '<' : ',';
		else if (key == kKeySlash) g_typed += shift ? '?' : '/';
		else if (key == kKeyBackslash) g_typed += shift ? '|' : '\\';
		else if (key == kKeyEqual) g_typed += shift ? '+' : '=';
		else if (key == kKeySemicolon) g_typed += shift ? ':' : ';';
		else if (key == kKeyApostrophe) g_typed += shift ? '"' : '\'';
		else if (key == kKeyLBracket) g_typed += shift ? '{' : '[';
		else if (key == kKeyRBracket) g_typed += shift ? '}' : ']';
	}
	if (g_puppet) {
		g_mouseX = g_puppetX;
		g_mouseY = g_puppetY;
		g_down[kMouseLeft] = g_puppetLeft;
		g_wheel = g_puppetWheel;
		g_typed = g_puppetTyped;
		g_puppetWheel = 0;
		g_puppetTyped.clear();
	}
}

// ---- hooks ----
// (__fastcall stands in for __thiscall: `self` arrives in ecx, edx is unused)
void __fastcall PaintHook(void *self, void *, int mode) {
	Passing call(g_paint);
	reinterpret_cast<void(__thiscall *)(void *, int)>(call.next)(self, mode);
	if (!call.own || !g_ready || !(mode & kPaintUiPanels) || !g_draw) return;
	perf::g_counters.frames++;
	perf::Scope timed(perf::kPaint);
	g_startDrawing(g_surface);
	Call<void, int *, int *>(g_surface, slot::GetScreenSize, &g_width, &g_height);
	if (g_height != g_fontScreenHeight) RebuildFonts();
	PollInput();
	g_draw();
	g_finishDrawing();
}

void ShowCursor(bool show) {
	void *context = Call<void *, int>(g_engineClient, slot::GetInputContext, 0);
	if (context) Call<void, void *, bool>(g_inputStack, slot::SetCursorVisible, context, show);
	g_cursorShown = show;
}

void __fastcall LockCursorHook(void *self, void *) {
	Passing call(g_lockCursor);
	if (call.own && g_ready && g_capture) {
		Call<void>(g_surface, slot::UnlockCursor);
		ShowCursor(true);
		return;
	}
	if (call.own && g_ready && g_cursorShown) ShowCursor(false);
	reinterpret_cast<void(__thiscall *)(void *)>(call.next)(self);
}

// The strafe keys held now, from the bindings the game passes with every key event (whatever keys they are bound
// to): for each button, the direction it went down for (kStrafeLeft, kStrafeRight) or 0. Kept per button rather
// than as a count of keys per direction, which a single release that never arrives here would leave wrong for
// good; and each frame a button the input system says is up is dropped (DropReleasedStrafeKeys).
constexpr int kStrafeButtons = 512;  // (above every ButtonCode_t, controllers' buttons included)
std::atomic<unsigned char> g_strafeButton[kStrafeButtons];

void NoteStrafeKey(int down, int key, const char *binding) {
	if (key < 0 || key >= kStrafeButtons) return;
	int direction = !down || !binding ? 0 : !strcmp(binding, "+moveleft") ? kStrafeLeft : !strcmp(binding, "+moveright") ? kStrafeRight : 0;
	g_strafeButton[key] = static_cast<unsigned char>(direction);
}

// (the game doesn't tell the client of every release: not when it loses the keyboard to another window, say)
void DropReleasedStrafeKeys() {
	for (int key = 0; key < kStrafeButtons; key++)
		if (g_strafeButton[key] && !Call<bool, int>(g_input, slot::IsButtonDown, key)) g_strafeButton[key] = 0;
}

int __fastcall KeyEventHook(void *self, void *, int down, int key, const char *binding) {
	Passing call(g_keyEvent);
	if (call.own) {
		NoteStrafeKey(down, key, binding);
		if (g_ready && g_capture) return 0;
	}
	return reinterpret_cast<int(__thiscall *)(void *, int, int, const char *)>(call.next)(self, down, key, binding);
}

void __fastcall ButtonStateHook(void *self, void *, const void *event) {
	Passing call(g_buttonState);
	if (call.own && g_ready && g_capture) {
		// (other code sets VGUI's button state too: it is cleared rather than only kept from changing)
		int context = *reinterpret_cast<int *>(static_cast<char *>(self) + slot::InputContextField);
		Call<void, int>(self, slot::ResetInputContext, context);
		return;
	}
	reinterpret_cast<void(__thiscall *)(void *, const void *)>(call.next)(self, event);
}

// Every mouse move, click, wheel step and typed key on its way to VGUI's panels (the game's main menu, its pause
// menu). While the menu is open they stop here, so the menu behind it neither highlights nor clicks; the console
// still gets them when it is open over the menu. (True = handled: the engine's own Esc and console keys are dealt
// with before this, and key releases still reach the game.)
bool __fastcall InputEventHook(void *self, void *, const void *event) {
	Passing call(g_inputEvent);
	if (call.own && g_ready && g_capture && !ConsoleVisible()) {
		int context = *reinterpret_cast<int *>(static_cast<char *>(g_vguiInput) + slot::InputContextField);
		Call<void, int>(g_vguiInput, slot::ResetInputContext, context);
		return true;
	}
	return reinterpret_cast<bool(__thiscall *)(void *, const void *)>(call.next)(self, event);
}

// The target of a relative call whose 4-byte operand is at `operand` (after an E8 byte)
void *CallTarget(const unsigned char *operand) {
	if (operand[-1] != 0xE8) return nullptr;
	int32_t relative;
	memcpy(&relative, operand, sizeof(relative));
	return const_cast<unsigned char *>(operand) + 4 + relative;
}

}  // namespace

bool Init() {
	if (g_ready) return true;
	g_surface = GetInterface("vguimatsurface.dll", "VGUI_Surface031");
	g_input = GetInterface("inputsystem.dll", "InputSystemVersion001");
	g_inputStack = GetInterface("inputsystem.dll", "InputStackSystemVersion001");
	g_engineVgui = GetInterface("engine.dll", "VEngineVGui001");
	g_engineClient = GetInterface("engine.dll", "VEngineClient015");
	g_client = GetInterface("client.dll", "VClient016");
	g_vguiInput = GetInterface("vgui2.dll", "VGUI_Input005");
	if (!g_surface || !g_input || !g_inputStack || !g_engineVgui || !g_engineClient || !g_client || !g_vguiInput) {
		engine::Warn("In-game menu off: the game's drawing or input interfaces weren't found.\n");
		return false;
	}
	auto paintTraverse = static_cast<const unsigned char *>((*static_cast<void ***>(g_surface))[slot::PaintTraverseEx]);
	g_startDrawing = reinterpret_cast<StartDrawingFn>(CallTarget(paintTraverse + slot::StartDrawingCall));
	g_finishDrawing = reinterpret_cast<FinishDrawingFn>(CallTarget(paintTraverse + slot::FinishDrawingCall));
	if (!g_startDrawing || !g_finishDrawing) {
		engine::Warn("In-game menu off: this version of the game draws differently (no StartDrawing call found).\n");
		return false;
	}
	// Another plugin (SAR) may copy a function table holding these hooks and keep using it after Shutdown has put
	// ours back: the DLL then stays loaded for the rest of the game (its hooks pass every call straight on).
	HMODULE self;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&PaintHook), &self);
	if (!Install(g_paint, g_engineVgui, slot::Paint, reinterpret_cast<void *>(&PaintHook)) ||
	    !Install(g_lockCursor, g_surface, slot::LockCursor, reinterpret_cast<void *>(&LockCursorHook)) ||
	    !Install(g_keyEvent, g_client, slot::InKeyEvent, reinterpret_cast<void *>(&KeyEventHook)) ||
	    !Install(g_buttonState, g_vguiInput, slot::UpdateButtonState, reinterpret_cast<void *>(&ButtonStateHook)) ||
	    !Install(g_inputEvent, g_surface, slot::HandleInputEvent, reinterpret_cast<void *>(&InputEventHook))) {
		Shutdown();
		engine::Warn("In-game menu off: couldn't hook the game's drawing.\n");
		return false;
	}
	g_ready = true;
	return true;
}

void Shutdown() {
	g_capture = false;
	if (g_cursorShown && g_engineClient) ShowCursor(false);
	g_ready = false;
	Remove(g_inputEvent);
	Remove(g_buttonState);
	Remove(g_keyEvent);
	Remove(g_lockCursor);
	Remove(g_paint);
	g_draw = nullptr;
}

bool Ready() { return g_ready; }
void SetDrawCallback(void (*draw)()) { g_draw = draw; }

int ScreenWidth() { return g_width; }
int ScreenHeight() { return g_height; }
float Scale() { return g_height > 0 ? g_height / 1080.0f : 1.0f; }

void FillRect(int x, int y, int w, int h, Color c) {
	if (w <= 0 || h <= 0) return;
	Call<void, int, int, int, int>(g_surface, slot::DrawSetColor, c.r, c.g, c.b, c.a);
	Call<void, int, int, int, int>(g_surface, slot::DrawFilledRect, x, y, x + w, y + h);
}

void OutlineRect(int x, int y, int w, int h, Color c) {
	if (w <= 0 || h <= 0) return;
	Call<void, int, int, int, int>(g_surface, slot::DrawSetColor, c.r, c.g, c.b, c.a);
	Call<void, int, int, int, int>(g_surface, slot::DrawOutlinedRect, x, y, x + w, y + h);
}

void Line(int x0, int y0, int x1, int y1, Color c) {
	Call<void, int, int, int, int>(g_surface, slot::DrawSetColor, c.r, c.g, c.b, c.a);
	Call<void, int, int, int, int>(g_surface, slot::DrawLine, x0, y0, x1, y1);
}

void Gradient(int x, int y, int w, int h, Color from, Color to, bool horizontal) {
	if (w <= 0 || h <= 0) return;
	// two fades over each other: `from` fading out, `to` fading in
	Call<void, int, int, int, int>(g_surface, slot::DrawSetColor, from.r, from.g, from.b, 255);
	Call<void, int, int, int, int, unsigned, unsigned, bool>(g_surface, slot::DrawFilledRectFade, x, y, x + w, y + h, from.a, 0, horizontal);
	Call<void, int, int, int, int>(g_surface, slot::DrawSetColor, to.r, to.g, to.b, 255);
	Call<void, int, int, int, int, unsigned, unsigned, bool>(g_surface, slot::DrawFilledRectFade, x, y, x + w, y + h, 0, to.a, horizontal);
}

void RebuildFonts() {
	struct Spec {
		int size;
		int weight;
	};
	// (Bahnschrift: Windows' DIN-style face, which the launcher's lettering is close to; Tahoma where it's missing)
	static const Spec specs[] = {{20, 400}, {24, 400}, {24, 700}, {34, 700}, {80, 700}};
	g_fontScreenHeight = g_height;
	float scale = Scale();
	for (int i = 0; i < static_cast<int>(Font::Count); i++) {
		int tall = static_cast<int>(specs[i].size * scale + 0.5f);
		if (tall < 8) tall = 8;
		// (a font is made once and given its glyphs again for the new size: the game keeps every font made until it
		// quits, so a new one per change of resolution would pile up)
		if (!g_fonts[i]) g_fonts[i] = Call<unsigned long>(g_surface, slot::CreateFont);
		unsigned long font = g_fonts[i];
		bool ok = Call<bool, unsigned long, const char *, int, int, int, int, int, int, int>(
			g_surface, slot::SetFontGlyphSet, font, "Bahnschrift", tall, specs[i].weight, 0, 0, kFontAntialias, 0, 0);
		if (!ok)
			Call<bool, unsigned long, const char *, int, int, int, int, int, int, int>(g_surface, slot::SetFontGlyphSet, font, "Tahoma", tall,
			                                                                           specs[i].weight, 0, 0, kFontAntialias, 0, 0);
		g_fontHeight[i] = Call<int, unsigned long>(g_surface, slot::GetFontTall, font);
	}
}

int Text(int x, int y, Font font, Color c, const char *text) {
	if (!text || !*text) return 0;
	using Fn = void(__cdecl *)(void *, unsigned long, int, int, int, int, int, int, const char *, ...);
	reinterpret_cast<Fn>((*static_cast<void ***>(g_surface))[slot::DrawColoredText])(g_surface, g_fonts[static_cast<int>(font)], x, y, c.r, c.g,
	                                                                                    c.b, c.a, "%s", text);
	return TextWidth(font, text);
}

int TextWidth(Font font, const char *text) {
	if (!text || !*text) return 0;
	std::wstring wide = Wide(text);
	int w = 0, h = 0;
	Call<void, unsigned long, const wchar_t *, int *, int *>(g_surface, slot::GetTextSize, g_fonts[static_cast<int>(font)], wide.c_str(), &w, &h);
	return w;
}

int FontHeight(Font font) { return g_fontHeight[static_cast<int>(font)]; }

bool KeyDown(int key) { return key >= 0 && key < kButtonCount && g_down[key]; }
bool KeyPressed(int key) { return key >= 0 && key < kButtonCount && g_down[key] && !g_wasDown[key]; }
bool KeyReleased(int key) { return key >= 0 && key < kButtonCount && !g_down[key] && g_wasDown[key]; }
int WheelSteps() { return g_wheel; }
int MouseX() { return g_mouseX; }
int MouseY() { return g_mouseY; }
const std::string &TypedText() { return g_typed; }

void SetCapture(bool capture) {
	g_capture = capture;
	if (!capture && g_cursorShown) ShowCursor(false);
}
bool Captured() { return g_capture; }

void SetPuppet(bool on) {
	g_puppet = on;
	g_puppetLeft = false;
}

void PuppetInput(int x, int y, bool leftDown, int wheel, const std::string &typed) {
	g_puppetX = x;
	g_puppetY = y;
	g_puppetLeft = leftDown;
	g_puppetWheel += wheel;
	g_puppetTyped += typed;
}

bool Puppeted() { return g_puppet; }

bool ConsoleVisible() { return g_engineClient && Call<bool>(g_engineClient, slot::ConIsVisible); }

int StrafeKeys() {
	int held = 0;
	for (const std::atomic<unsigned char> &direction : g_strafeButton) held |= direction;
	return held;
}
bool InGame() { return engine::MapName()[0] != '\0'; }

}  // namespace gfx
