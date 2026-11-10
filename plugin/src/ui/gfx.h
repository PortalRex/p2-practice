// Drawing on the game's screen and reading the mouse and keyboard, for the plugin's own menu and HUD.
//
// The game draws its 2D interface (VGUI) through the material system's surface. Once per frame, after the game
// painted its panels, `Draw` callbacks get the screen: rectangles, lines and text, in pixels from the top left.
// Input is read by polling the game's input system each frame (keys and mouse buttons: held, pressed this frame,
// released this frame; the mouse wheel; the cursor). While the menu has the input (`SetCapture(true)`), the
// cursor is freed and shown, and neither the game nor its own menus see keys or clicks.
//
// This is where the plugin hooks the game (but for one slot in engine.cpp, see HoldClientCommands): six
// function-table slots (the end of VGUI's paint, the cursor lock, the client's key events, VGUI's button input,
// the surface's input events, the end of a view's 3D drawing), each patched in the table the object uses at that
// moment and put back by Shutdown.
// SAR hooks the same objects by giving them a copy of their table; patching the table in use keeps both working
// whichever of the two was loaded first.
#pragma once

#include <cstdint>
#include <string>

namespace gfx {

struct Color {
	uint8_t r = 255, g = 255, b = 255, a = 255;
	constexpr Color() = default;
	constexpr Color(int r_, int g_, int b_, int a_ = 255)
		: r(static_cast<uint8_t>(r_)), g(static_cast<uint8_t>(g_)), b(static_cast<uint8_t>(b_)), a(static_cast<uint8_t>(a_)) {}
	Color Alpha(int alpha) const { return Color(r, g, b, alpha); }
};

// The game's button codes (ButtonCode_t), as far as the menu reads them
enum Key : int {
	kKey0 = 1,   // ... kKey0 + 9
	kKeyA = 11,  // ... kKeyA + 25
	kKeyPad0 = 37,
	kKeyPadEnter = 51, kKeyPadDecimal = 52,
	kKeyLBracket = 53, kKeyRBracket, kKeySemicolon, kKeyApostrophe, kKeyBackquote, kKeyComma, kKeyPeriod, kKeySlash,
	kKeyBackslash, kKeyMinus, kKeyEqual, kKeyEnter, kKeySpace, kKeyBackspace, kKeyTab,
	kKeyEscape = 70,
	kKeyInsert = 72, kKeyDelete, kKeyHome, kKeyEnd, kKeyPageUp, kKeyPageDown,
	kKeyLShift = 79, kKeyRShift, kKeyLAlt, kKeyRAlt, kKeyLControl, kKeyRControl,
	kKeyUp = 88, kKeyLeft, kKeyDown, kKeyRight,
	kKeyF1 = 92,  // ... kKeyF1 + 11
	kMouseLeft = 107, kMouseRight, kMouseMiddle, kMouse4, kMouse5, kMouseWheelUp, kMouseWheelDown,
	kButtonCount = 114,
};

// Finds the game's interfaces and installs the hooks. False (with a reason printed) when something is missing:
// the plugin then works without its menu and HUD, as before.
bool Init();
void Shutdown();
bool Ready();

// Called once per frame with the screen to draw on (after the game's own panels)
void SetDrawCallback(void (*draw)());

// ---- drawing (only inside the draw callback) ----
int ScreenWidth();
int ScreenHeight();
void FillRect(int x, int y, int w, int h, Color color);
void OutlineRect(int x, int y, int w, int h, Color color);
void Line(int x0, int y0, int x1, int y1, Color color);
// a rectangle fading from `from` to `to`, top to bottom (or left to right)
void Gradient(int x, int y, int w, int h, Color from, Color to, bool horizontal);

enum class Font { Small, Normal, Bold, Title, Huge, Count };
// Text is UTF-8. `Text` draws with the top left at (x, y) and returns the width drawn.
int Text(int x, int y, Font font, Color color, const char *text);
int TextWidth(Font font, const char *text);
int FontHeight(Font font);
// Fonts are sized for a 1080-pixel-high screen and scaled to the real one; called again when the size changes.
void RebuildFonts();
float Scale();  // screen height / 1080

// ---- drawing in the level ----
// Once per view of the level (each split-screen half), after the game drew the level and before its 2D
// interface, the world callback may draw in the level itself: flat coloured shapes in world coordinates, hidden
// behind walls like anything else there. Everything given in one frame goes to the graphics card in a few
// batches, whatever its amount.
struct WorldVertex {
	float x = 0, y = 0, z = 0;
	Color color;
};
void SetWorldCallback(void (*draw)());
// False when the game's 3D drawing wasn't found (said once in the console): nothing is drawn in the level
bool WorldReady();
// Four-cornered shapes, four vertices each, going round the shape; seen from both sides. Only inside the world
// callback.
void WorldQuads(const WorldVertex *corners, int quads);

// ---- input (read inside the draw callback) ----
bool KeyDown(int key);
bool KeyPressed(int key);   // went down since the last frame
bool KeyReleased(int key);  // went up since the last frame
int WheelSteps();           // + up, - down, since the last frame
int MouseX();
int MouseY();
// The characters typed since the last frame (letters, digits, punctuation, space), for text boxes
const std::string &TypedText();

// While true the menu owns the mouse and keyboard: the cursor is free and visible, the game and its menus get
// no keys or clicks (the menu's own key, read by polling, still works).
void SetCapture(bool capture);
bool Captured();

// A stand-in for the mouse and the keyboard's typing, driven by the plugin itself (the menu's scripted tour, for
// showing it): while on, the cursor, the left button, the wheel and typed text come from here. Nothing is sent to
// Windows or the game; only the plugin's own menu reads it.
void SetPuppet(bool on);
void PuppetInput(int x, int y, bool leftDown, int wheel, const std::string &typed);
bool Puppeted();

bool ConsoleVisible();  // the game's console is open (keys typed are the console's)

// The strafe keys the player holds now (whatever keys +moveleft and +moveright are bound to)
constexpr int kStrafeLeft = 1, kStrafeRight = 2;
int StrafeKeys();
bool InGame();          // a level is loaded (else the game's main menu is up)

}  // namespace gfx
