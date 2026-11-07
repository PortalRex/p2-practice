// The look: test-chamber signage. Off-white panels on a cool grey field, near-black ink, thin rules, a condensed
// signage face, pictograms in rounded squares - and the two portal colours, used only where they mean a robot.
// Everything is drawn here with the launcher's own colours and shapes: nothing depends on the system colours,
// and nothing comes from the game's files. The dark mode is the same signage after lights-out: dark panels,
// pale ink, the portal colours unchanged.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.Runtime.CompilerServices;
using System.Windows.Forms;

namespace P2Practice {
	static class Theme {
		public static bool IsDark { get; private set; }
		public static Color Field = Color.FromArgb(0xCF, 0xD3, 0xD6);   // behind the panels
		public static Color Panel = Color.FromArgb(0xF2, 0xF2, 0xF0);
		public static Color Input = Color.FromArgb(0xFF, 0xFF, 0xFF);   // where the player types
		public static Color Ink = Color.FromArgb(0x1A, 0x1A, 0x1A);
		public static Color Grey = Color.FromArgb(0x70, 0x74, 0x78);    // secondary text
		public static Color Pale = Color.FromArgb(0xA9, 0xAD, 0xB0);    // dimmed text, unlit glyphs
		public static Color Unlit = Color.FromArgb(0xDC, 0xDD, 0xDC);   // a pictogram square that is off
		public static Color Line = Color.FromArgb(0xC9, 0xCB, 0xCB);    // light dividers
		public static Color Stripe = Color.FromArgb(0xEA, 0xEA, 0xE8);  // every other row
		public static Color Dark = Color.FromArgb(0x1C, 0x1D, 0x1F);    // the band along the top (dark in both modes)
		public static Color Pressed = Color.Black;                      // a lit sign pressed down
		public static readonly Color Blue = Color.FromArgb(0x00, 0xA2, 0xFF);    // ATLAS, and the player's first portal
		public static readonly Color Orange = Color.FromArgb(0xFF, 0x9A, 0x00);  // P-body
		public static Color Alert = Color.FromArgb(0xB3, 0x26, 0x1E);   // something is wrong
		static Color lightText = Color.White, darkText = Color.FromArgb(0x1A, 0x1A, 0x1A);

		// Switches the whole palette. Everything drawn afterwards takes the new colours (the window is built again).
		public static void Apply(bool dark) {
			IsDark = dark;
			if (dark) {
				Field = Color.FromArgb(0x12, 0x13, 0x15);
				Panel = Color.FromArgb(0x1F, 0x21, 0x24);
				Input = Color.FromArgb(0x2B, 0x2E, 0x31);
				Ink = Color.FromArgb(0xEC, 0xEC, 0xE8);
				Grey = Color.FromArgb(0x9C, 0xA0, 0xA4);
				Pale = Color.FromArgb(0x62, 0x66, 0x6A);
				Unlit = Color.FromArgb(0x36, 0x39, 0x3D);
				Line = Color.FromArgb(0x3A, 0x3D, 0x41);
				Stripe = Color.FromArgb(0x25, 0x27, 0x2B);
				Dark = Color.FromArgb(0x0C, 0x0D, 0x0E);
				Pressed = Color.FromArgb(0xC8, 0xC8, 0xC4);
				Alert = Color.FromArgb(0xE8, 0x5A, 0x50);
				lightText = Ink;
				darkText = Field;
			} else {
				Field = Color.FromArgb(0xCF, 0xD3, 0xD6);
				Panel = Color.FromArgb(0xF2, 0xF2, 0xF0);
				Input = Color.FromArgb(0xFF, 0xFF, 0xFF);
				Ink = Color.FromArgb(0x1A, 0x1A, 0x1A);
				Grey = Color.FromArgb(0x70, 0x74, 0x78);
				Pale = Color.FromArgb(0xA9, 0xAD, 0xB0);
				Unlit = Color.FromArgb(0xDC, 0xDD, 0xDC);
				Line = Color.FromArgb(0xC9, 0xCB, 0xCB);
				Stripe = Color.FromArgb(0xEA, 0xEA, 0xE8);
				Dark = Color.FromArgb(0x1C, 0x1D, 0x1F);
				Pressed = Color.Black;
				Alert = Color.FromArgb(0xB3, 0x26, 0x1E);
				lightText = Color.White;
				darkText = Ink;
			}
		}

		public static Color Mix(Color a, Color b, float t) {
			return Color.FromArgb((int)(a.R + (b.R - a.R) * t), (int)(a.G + (b.G - a.G) * t), (int)(a.B + (b.B - a.B) * t));
		}

		// what reads on a fill of this colour
		public static Color On(Color fill) { return fill.GetBrightness() > 0.55f || fill == Orange ? darkText : lightText; }
		// what reads on the ink (a selected row, a lit sign): white, or the dark field in dark mode
		public static Color OnInk { get { return On(Ink); } }
	}

	// The fonts and the scale the window is drawn at, and the portal colour of the moment. One per build of the
	// window: a move to a monitor with another scale makes a new one.
	class Look : IDisposable {
		public readonly float S;                 // pixels per unit (1 = 100%)
		public Color Accent = Theme.Blue;        // the robot the player is about to be: blue, or orange for P-body
		public ToolTip Tips;
		public readonly Font Body, Small, Strong, Caps, Brand, Button, Title, Large, Huge;

		public Look(float scale) {
			S = Ui.Scale = scale;
			Body = Face(13.5f, false);
			Small = Face(12f, false);
			Strong = Face(14f, true);
			Caps = Face(11.5f, true);    // small capitals, letter-spaced: captions and column heads
			Brand = Face(16f, true);
			Button = Face(22f, true);
			Title = Face(26f, true);     // the map's name
			Large = Face(42f, true);     // a time too long for the huge numerals
			Huge = Face(62f, true);      // the run's time, as a chamber sign shows its number
		}

		// The signage face: Bahnschrift's condensed widths (it ships with Windows 10 and 11), else something narrow.
		Font Face(float pixels, bool strong) {
			foreach (var name in strong ? new[] { "Bahnschrift SemiBold Condensed", "Arial Narrow", "Segoe UI Semibold" }
			                            : new[] { "Bahnschrift SemiCondensed", "Arial Narrow", "Segoe UI" }) {
				var f = new Font(name, pixels * S, strong && name == "Arial Narrow" ? FontStyle.Bold : FontStyle.Regular, GraphicsUnit.Pixel);
				if (string.Equals(f.Name, name, StringComparison.OrdinalIgnoreCase)) return f; // (a missing font comes back as another)
				f.Dispose();
			}
			return new Font(FontFamily.GenericSansSerif, pixels * S, strong ? FontStyle.Bold : FontStyle.Regular, GraphicsUnit.Pixel);
		}

		public int P(float v) { return (int)Math.Round(v * S); }

		public void Dispose() {
			foreach (var f in new[] { Body, Small, Strong, Caps, Brand, Button, Title, Large, Huge }) f.Dispose();
		}
	}

	// Drawing helpers: text, letter-spaced capitals, rounded shapes.
	//
	// Text is drawn two ways. Reading sizes go through GDI, like the text in a text box: hinted to the pixel grid,
	// evenly spaced and sharp. Display sizes (the numerals, the map's name) go through GDI+, which smooths their
	// big curves in both directions. Measuring uses the same engine as drawing, so what was measured fits.
	static class Ui {
		static readonly Graphics measurer = Measurer();
		static readonly Dictionary<int, StringFormat> formats = new Dictionary<int, StringFormat>();
		static readonly ConditionalWeakTable<Font, Dictionary<char, float>> advances = new ConditionalWeakTable<Font, Dictionary<char, float>>();
		const TextFormatFlags Plain = TextFormatFlags.NoPrefix | TextFormatFlags.NoPadding | TextFormatFlags.PreserveGraphicsClipping;

		static Graphics Measurer() {
			var g = Graphics.FromImage(new Bitmap(1, 1));
			g.TextRenderingHint = TextRenderingHint.AntiAlias;
			return g;
		}

		public static void Prepare(Graphics g) {
			g.SmoothingMode = SmoothingMode.AntiAlias;
			g.PixelOffsetMode = PixelOffsetMode.Half;           // whole-pixel rectangles stay sharp
			g.TextRenderingHint = TextRenderingHint.AntiAlias;
		}

		// what counts as a reading size at the scale the window is drawn at (the fonts are sized in pixels)
		public static float Scale = 1;
		static bool Reading(Font font) { return font.Size < 20f * Scale; }

		static StringFormat Format(ContentAlignment align, bool wrap) {
			int key = (int)align * 2 + (wrap ? 1 : 0);
			StringFormat f;
			if (formats.TryGetValue(key, out f)) return f;
			f = (StringFormat)StringFormat.GenericTypographic.Clone(); // (no padding around the text)
			f.FormatFlags = (f.FormatFlags | StringFormatFlags.MeasureTrailingSpaces | StringFormatFlags.NoClip) & ~StringFormatFlags.LineLimit;
			if (!wrap) f.FormatFlags |= StringFormatFlags.NoWrap;
			f.Trimming = StringTrimming.EllipsisCharacter;
			int a = (int)align;
			f.Alignment = (a & 0x111) != 0 ? StringAlignment.Near : (a & 0x222) != 0 ? StringAlignment.Center : StringAlignment.Far;
			f.LineAlignment = a <= 0x004 ? StringAlignment.Near : a <= 0x040 ? StringAlignment.Center : StringAlignment.Far;
			return formats[key] = f;
		}

		static TextFormatFlags Flags(ContentAlignment align, bool wrap) {
			int a = (int)align;
			var f = Plain | ((a & 0x111) != 0 ? TextFormatFlags.Left : (a & 0x222) != 0 ? TextFormatFlags.HorizontalCenter : TextFormatFlags.Right);
			if (wrap) return f | TextFormatFlags.WordBreak | TextFormatFlags.TextBoxControl; // (several lines: from the top)
			return f | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis | (a <= 0x004 ? TextFormatFlags.Top : a <= 0x040 ? TextFormatFlags.VerticalCenter : TextFormatFlags.Bottom);
		}

		// text in a box; what doesn't fit ends in "…"
		public static void Text(Graphics g, string text, Font font, Color color, RectangleF box, ContentAlignment align = ContentAlignment.MiddleLeft, bool wrap = false) {
			if (string.IsNullOrEmpty(text) || box.Width <= 0 || box.Height <= 0) return;
			if (Reading(font)) TextRenderer.DrawText(g, text, font, Rectangle.Round(box), color, Flags(align, wrap));
			else using (var b = new SolidBrush(color)) g.DrawString(text, font, b, box, Format(align, wrap));
		}

		public static float Width(string text, Font font) {
			if (string.IsNullOrEmpty(text)) return 0;
			if (Reading(font)) return TextRenderer.MeasureText(text, font, new Size(int.MaxValue, int.MaxValue), Plain | TextFormatFlags.SingleLine).Width;
			return measurer.MeasureString(text, font, PointF.Empty, Format(ContentAlignment.TopLeft, false)).Width;
		}

		public static SizeF Wrapped(string text, Font font, float width) {
			if (Reading(font)) return TextRenderer.MeasureText(text ?? "", font, new Size((int)width, 0), Plain | TextFormatFlags.WordBreak | TextFormatFlags.TextBoxControl);
			return measurer.MeasureString(text ?? "", font, new SizeF(width, 10000), Format(ContentAlignment.TopLeft, true));
		}

		static float Advance(char c, Font font) {
			var known = advances.GetOrCreateValue(font);
			float w;
			// (two of the letter less one: what a measurement adds around the text cancels out)
			if (!known.TryGetValue(c, out w)) known[c] = w = Width(new string(c, 2), font) - Width(c.ToString(), font);
			return w;
		}

		// the width of capitals set with extra space between the letters
		public static float TrackedWidth(string text, Font font, float tracking) {
			tracking = Whole(font, tracking);
			float w = 0;
			foreach (char c in text) w += Advance(c, font) + tracking;
			return text.Length > 0 ? w - tracking : 0;
		}

		// (reading sizes sit on whole pixels: so must the space between their letters, or the gaps come out uneven)
		static float Whole(Font font, float tracking) { return Reading(font) ? (float)Math.Max(1, Math.Round(tracking)) : tracking; }

		// Letter-spaced text (signs set their capitals a little apart). Returns where it ended.
		public static float Tracked(Graphics g, string text, Font font, Color color, RectangleF box, float tracking, ContentAlignment align = ContentAlignment.MiddleLeft) {
			if (string.IsNullOrEmpty(text)) return box.X;
			float width = TrackedWidth(text, font, tracking);
			tracking = Whole(font, tracking);
			int a = (int)align;
			float x = (a & 0x111) != 0 ? box.X : (a & 0x222) != 0 ? box.X + (box.Width - width) / 2 : box.Right - width;
			var vertical = a <= 0x004 ? ContentAlignment.TopLeft : a <= 0x040 ? ContentAlignment.MiddleLeft : ContentAlignment.BottomLeft;
			foreach (char c in text) {
				float w = Advance(c, font);
				if (x + w > box.Right + 0.5f) break; // (captions are short; one that doesn't fit is cut)
				if (c != ' ') Text(g, c.ToString(), font, color, new RectangleF((float)Math.Round(x), box.Y, w + 6, box.Height), vertical);
				x += w + tracking;
			}
			return x - tracking;
		}

		public static GraphicsPath Round(RectangleF r, float radius) {
			var p = new GraphicsPath();
			float d = Math.Min(radius * 2, Math.Min(r.Width, r.Height));
			if (d <= 0.01f) { p.AddRectangle(r); return p; }
			p.AddArc(r.X, r.Y, d, d, 180, 90);
			p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
			p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
			p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
			p.CloseFigure();
			return p;
		}

		public static void FillRound(Graphics g, Color fill, RectangleF r, float radius) {
			using (var p = Round(r, radius)) using (var b = new SolidBrush(fill)) g.FillPath(b, p);
		}

		// an outline of this thickness just inside the box
		public static void StrokeRound(Graphics g, Color color, RectangleF r, float radius, float thickness) {
			r.Inflate(-thickness / 2, -thickness / 2);
			using (var p = Round(r, Math.Max(0, radius - thickness / 2))) using (var pen = new Pen(color, thickness)) g.DrawPath(pen, p);
		}

		public static void Fill(Graphics g, Color fill, RectangleF r) {
			using (var b = new SolidBrush(fill)) g.FillRectangle(b, r);
		}
	}

	// The pictograms: plain solid glyphs, drawn on a 24 x 24 grid and scaled to the box. A pictogram's "hole"
	// colour is what shows through it (the square it sits on).
	static class Pict {
		// a lit or unlit pictogram square, as on a chamber sign
		public static void Tile(Graphics g, string name, RectangleF box, Color fill, Color glyph) {
			Ui.FillRound(g, fill, box, box.Width * 0.17f);
			Draw(g, name, box, glyph, fill);
		}

		public static void Draw(Graphics g, string name, RectangleF box, Color ink, Color hole) {
			var state = g.Save();
			g.TranslateTransform(box.X, box.Y);
			g.ScaleTransform(box.Width / 24f, box.Height / 24f);
			using (var b = new SolidBrush(ink))
			using (var h = new SolidBrush(hole))
			using (var pen = new Pen(ink, 2.1f) { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round }) {
				switch (name) {
					case "ghost": // the recorded players
						using (var p = new GraphicsPath()) {
							p.AddArc(5, 4, 14, 14, 180, 180);
							p.AddLines(new[] { new PointF(19, 20), new PointF(16.7f, 17.6f), new PointF(14.3f, 20), new PointF(12, 17.6f), new PointF(9.7f, 20), new PointF(7.3f, 17.6f), new PointF(5, 20) });
							p.CloseFigure();
							g.FillPath(b, p);
						}
						g.FillEllipse(h, 8.1f, 9.4f, 2.7f, 3.3f);
						g.FillEllipse(h, 13.2f, 9.4f, 2.7f, 3.3f);
						break;
					case "clock":
						g.DrawEllipse(pen, 4.5f, 4.5f, 15, 15);
						g.DrawLine(pen, 12, 12, 12, 7.6f);
						g.DrawLine(pen, 12, 12, 15.3f, 13.9f);
						break;
					case "keys": // a cluster of key caps
						foreach (var k in new[] { new PointF(9, 4.2f), new PointF(2.4f, 11.2f), new PointF(9, 11.2f), new PointF(15.6f, 11.2f) })
							using (var p = Ui.Round(new RectangleF(k.X, k.Y, 6, 6), 1.3f)) g.FillPath(b, p);
						break;
					case "line": // the route through the chamber
						g.DrawLines(pen, new[] { new PointF(4.6f, 18.4f), new PointF(9.2f, 10), new PointF(14.6f, 14.4f), new PointF(19.4f, 5.6f) });
						g.FillEllipse(b, 2.4f, 16.2f, 4.4f, 4.4f);
						g.FillEllipse(b, 17.2f, 3.4f, 4.4f, 4.4f);
						break;
					case "bot": // a machine plays the partner
						g.DrawLine(pen, 12, 7.6f, 12, 4.6f);
						g.FillEllipse(b, 10.5f, 2.5f, 3, 3);
						using (var p = Ui.Round(new RectangleF(4.5f, 7.5f, 15, 12.5f), 3.2f)) g.FillPath(b, p);
						g.FillEllipse(h, 8.5f, 10.3f, 7, 7);
						g.FillEllipse(b, 10.6f, 12.4f, 2.8f, 2.8f);
						break;
					case "people": // a person plays the partner
						g.FillEllipse(b, 13.6f, 5.2f, 5.6f, 5.6f);
						g.FillPie(b, 11.4f, 12.2f, 10, 12, 180, 180);
						g.FillEllipse(h, 3.6f, 3.2f, 8.8f, 8.8f);
						g.FillPie(h, 1.2f, 10.8f, 13.6f, 15, 180, 180);
						g.FillEllipse(b, 5, 4.6f, 6, 6);
						g.FillPie(b, 2.5f, 12, 11, 12.6f, 180, 180);
						break;
					case "loop": { // start again
						g.DrawArc(pen, 5, 5, 14, 14, -60, 292);
						double end = (-60 + 292) * Math.PI / 180;
						var at = new PointF(12 + 7 * (float)Math.Cos(end), 12 + 7 * (float)Math.Sin(end));
						var along = new PointF(-(float)Math.Sin(end), (float)Math.Cos(end));
						var across = new PointF((float)Math.Cos(end), (float)Math.Sin(end));
						g.FillPolygon(b, new[] { new PointF(at.X + along.X * 4.2f, at.Y + along.Y * 4.2f),
							new PointF(at.X + across.X * 3.4f - along.X * 0.6f, at.Y + across.Y * 3.4f - along.Y * 0.6f),
							new PointF(at.X - across.X * 3.4f - along.X * 0.6f, at.Y - across.Y * 3.4f - along.Y * 0.6f) });
						break;
					}
					case "play": // practice: play it yourself
						using (var p = new GraphicsPath()) {
							p.AddPolygon(new[] { new PointF(8.6f, 5.4f), new PointF(19, 12), new PointF(8.6f, 18.6f) });
							using (var edge = new Pen(ink, 1.6f) { LineJoin = LineJoin.Round }) g.DrawPath(edge, p);
							g.FillPath(b, p);
						}
						break;
					case "eye": // watch
						using (var p = new GraphicsPath()) {
							p.AddBezier(2.6f, 12, 7, 5.4f, 17, 5.4f, 21.4f, 12);
							p.AddBezier(21.4f, 12, 17, 18.6f, 7, 18.6f, 2.6f, 12);
							g.DrawPath(pen, p);
						}
						g.FillEllipse(b, 9, 9, 6, 6);
						break;
					case "search":
						g.DrawEllipse(pen, 4.6f, 4.6f, 10, 10);
						using (var handle = new Pen(ink, 2.7f) { StartCap = LineCap.Round, EndCap = LineCap.Round }) g.DrawLine(handle, 13.6f, 13.6f, 19, 19);
						break;
					case "minus":
						g.DrawLine(pen, 7, 12, 17, 12);
						break;
					case "plus":
						g.DrawLine(pen, 7, 12, 17, 12);
						g.DrawLine(pen, 12, 7, 12, 17);
						break;
					case "atlas": // the round robot
						g.DrawLine(pen, 9, 18, 7.6f, 21.6f);
						g.DrawLine(pen, 15, 18, 16.4f, 21.6f);
						g.FillEllipse(b, 4.6f, 3.6f, 14.8f, 14.8f);
						g.FillEllipse(h, 8.6f, 7.6f, 6.8f, 6.8f);
						g.FillEllipse(b, 10.6f, 9.6f, 2.8f, 2.8f);
						break;
					case "pbody": // the tall robot
						g.DrawLine(pen, 10.2f, 16, 8.6f, 21.6f);
						g.DrawLine(pen, 13.8f, 16, 15.4f, 21.6f);
						using (var p = Ui.Round(new RectangleF(7.4f, 2.4f, 9.2f, 15), 4.6f)) g.FillPath(b, p);
						g.FillEllipse(h, 9.1f, 4.9f, 5.8f, 5.8f);
						g.FillEllipse(b, 10.8f, 6.6f, 2.4f, 2.4f);
						break;
					case "portal": // an upright oval
						using (var ring = new Pen(ink, 2.6f)) g.DrawEllipse(ring, 7, 2.6f, 10, 18.8f);
						break;
					case "iris": // a shutter of six blades: the launcher's own mark
						for (int i = 0; i < 6; i++) {
							double a = (i * 60 - 90) * Math.PI / 180, wide = 47 * Math.PI / 180, twist = 34 * Math.PI / 180;
							g.FillPolygon(b, new[] { Polar(10.6f, a), Polar(10.6f, a + wide), Polar(4.4f, a + wide + twist), Polar(4.4f, a + twist) });
						}
						break;
				}
			}
			g.Restore(state);
		}

		static PointF Polar(float radius, double angle) {
			return new PointF(12 + radius * (float)Math.Cos(angle), 12 + radius * (float)Math.Sin(angle));
		}
	}
}
