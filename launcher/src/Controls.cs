// The window's controls, drawn in the sign style (see Theme.cs): buttons, pictogram options, text fields, a
// number stepper, the lists, and the menu's renderer. They paint themselves entirely, so they look the same
// whatever the system's colours are; each keeps a visible keyboard focus cue and works from the keyboard.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using Timer = System.Windows.Forms.Timer;

namespace P2Practice {
	// What every drawn control shares: no flicker, repaint on resize and on focus or enabled changes.
	class SignControl : Control {
		protected readonly Look L;

		public SignControl(Look look, bool takesFocus) {
			L = look;
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
			SetStyle(ControlStyles.Selectable, takesFocus);
			TabStop = takesFocus;
			BackColor = Theme.Panel;
		}

		protected int P(float v) { return L.P(v); }

		// has the keyboard: draw the focus cue. (A screenshot can ask for every cue at once, to look at them:
		// a window that is never shown never has the focus.)
		public static bool AllFocusCues;
		protected bool KeyFocus { get { return Focused || AllFocusCues; } }

		protected override void OnEnabledChanged(EventArgs e) { base.OnEnabledChanged(e); Invalidate(); }
		protected override void OnGotFocus(EventArgs e) { base.OnGotFocus(e); Invalidate(); }
		protected override void OnLostFocus(EventArgs e) { base.OnLostFocus(e); Invalidate(); }
		protected override void OnTextChanged(EventArgs e) { base.OnTextChanged(e); Invalidate(); }
	}

	// A line (or a few) of text. Text that doesn't fit ends in "…"; the whole of it is then the tooltip.
	class Words : SignControl {
		public Font Face;
		public ContentAlignment Align = ContentAlignment.MiddleLeft;
		public bool Wrap;
		public bool Lamp; // a small square before the words, in their colour (the status line's)

		public Words(Look look, string text, Font face, Color color) : base(look, false) {
			Face = face;
			ForeColor = color;
			Text = text;
			Height = face.Height + P(2);
			Width = (int)Math.Ceiling(Ui.Width(text, face)) + P(2);
		}

		// as wide as its text
		public void Fit() { Width = (int)Math.Ceiling(Ui.Width(Text, Face)) + P(2); }

		protected override void OnPaint(PaintEventArgs e) {
			Ui.Prepare(e.Graphics);
			e.Graphics.Clear(BackColor);
			RectangleF box = ClientRectangle;
			if (Lamp && Text.Length > 0) {
				float size = P(8);
				Ui.Fill(e.Graphics, ForeColor, new RectangleF(0, (Height - size) / 2f, size, size));
				box = new RectangleF(size + P(8), 0, Width - size - P(8), Height);
			}
			Ui.Text(e.Graphics, Text, Face, ForeColor, box, Align, Wrap);
		}
	}

	// A caption in small letter-spaced capitals.
	class Caption : SignControl {
		public Caption(Look look, string text) : base(look, false) {
			Text = text;
			Size = new Size((int)Math.Ceiling(Ui.TrackedWidth(text, look.Caps, Tracking)) + P(2), P(26));
		}

		float Tracking { get { return L.S; } }

		protected override void OnPaint(PaintEventArgs e) {
			Ui.Prepare(e.Graphics);
			e.Graphics.Clear(BackColor);
			Ui.Tracked(e.Graphics, Text, L.Caps, Enabled ? Theme.Grey : Theme.Pale, ClientRectangle, Tracking);
		}
	}

	// A panel with a thin outline: the off-white boards the window is made of.
	class Card : Panel {
		public Card() {
			BackColor = Theme.Panel;
			SetStyle(ControlStyles.ResizeRedraw | ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint, true);
		}

		protected override void OnPaint(PaintEventArgs e) {
			base.OnPaint(e);
			using (var pen = new Pen(Theme.Ink)) e.Graphics.DrawRectangle(pen, 0, 0, Width - 1, Height - 1);
		}
	}

	// A button. The primary one (Launch) is dark, with an edge that glows in the portal colour of the moment.
	class SignButton : SignControl {
		public bool Primary;
		public string Icon;
		bool hot, down;

		public SignButton(Look look, string text) : base(look, true) {
			Text = AccessibleName = text;
			AccessibleRole = AccessibleRole.PushButton;
			Cursor = Cursors.Hand;
			Size = new Size((int)Math.Ceiling(Ui.TrackedWidth(text, look.Caps, look.S)) + P(30), P(28));
		}

		protected override void OnMouseEnter(EventArgs e) { hot = true; Invalidate(); base.OnMouseEnter(e); }
		protected override void OnMouseLeave(EventArgs e) { hot = down = false; Invalidate(); base.OnMouseLeave(e); }
		protected override void OnMouseDown(MouseEventArgs e) { if (e.Button == MouseButtons.Left) { down = true; Focus(); Invalidate(); } base.OnMouseDown(e); }
		protected override void OnMouseUp(MouseEventArgs e) { down = false; Invalidate(); base.OnMouseUp(e); }
		protected override void OnKeyDown(KeyEventArgs e) {
			if (e.KeyCode == Keys.Space || e.KeyCode == Keys.Enter) { OnClick(EventArgs.Empty); e.Handled = true; }
			base.OnKeyDown(e);
		}
		protected override bool IsInputKey(Keys key) { return key == Keys.Enter || base.IsInputKey(key); }

		protected override void OnPaint(PaintEventArgs e) {
			var g = e.Graphics;
			Ui.Prepare(g);
			g.Clear(BackColor);
			var r = new RectangleF(0, 0, Width, Height);
			float radius = P(3);
			if (Primary) {
				var fill = !Enabled ? Theme.Mix(Theme.Panel, Theme.Ink, 0.22f) : down ? Color.Black : hot ? Theme.Mix(Theme.Ink, Color.White, 0.10f) : Theme.Ink;
				Ui.FillRound(g, fill, r, radius);
				if (Enabled) {
					// the edge: the portal colour, fading inwards like light off a portal's rim
					float w = Math.Max(2, P(2)), reach = P(hot ? 13 : 10);
					for (float inset = w; inset < w + reach; inset += 1) {
						float fade = 1 - (inset - w) / reach;
						Ui.StrokeRound(g, Color.FromArgb((int)(120 * fade * fade), L.Accent), RectangleF.Inflate(r, -inset, -inset), Math.Max(0, radius - inset), 1);
					}
					Ui.StrokeRound(g, L.Accent, r, radius, w);
				}
				var ink = Enabled ? Color.White : Theme.Panel;
				float tracking = L.S * 3.2f, text = Ui.TrackedWidth(Text, L.Button, tracking), mark = Height * 0.46f, gap = P(12);
				float x = (Width - (mark * 0.6f + gap + text)) / 2;
				// an upright portal beside the word
				Pict.Draw(g, "portal", new RectangleF(x - mark * 0.2f, (Height - mark) / 2, mark, mark), Enabled ? L.Accent : Theme.Panel, fill);
				Ui.Tracked(g, Text, L.Button, ink, new RectangleF(x + mark * 0.6f + gap, 0, text + 8, Height), tracking);
				if (KeyFocus && Enabled) Ui.StrokeRound(g, Color.FromArgb(200, Color.White), RectangleF.Inflate(r, -P(5), -P(5)), radius, Math.Max(1, P(1)));
			} else {
				var fill = !Enabled ? Theme.Panel : down ? Theme.Ink : hot ? Theme.Mix(Theme.Panel, Theme.Ink, 0.10f) : Theme.Panel;
				var ink = !Enabled ? Theme.Pale : down ? Color.White : Theme.Ink;
				Ui.FillRound(g, fill, r, radius);
				Ui.StrokeRound(g, Enabled ? Theme.Ink : Theme.Pale, r, radius, Math.Max(1, P(1)) * (KeyFocus ? 2 : 1));
				float x = P(12);
				if (Icon != null) {
					float mark = P(16);
					Pict.Draw(g, Icon, new RectangleF(P(8), (Height - mark) / 2, mark, mark), ink, fill);
					x = P(8) + mark + P(5);
				}
				// (with a pictogram the word follows it; without one it sits in the middle)
				Ui.Tracked(g, Text, L.Caps, ink, Icon != null ? new RectangleF(x, 0, Width - x, Height) : r, L.S, Icon != null ? ContentAlignment.MiddleLeft : ContentAlignment.MiddleCenter);
			}
		}
	}

	// A choice shown as a sign shows a hazard: a pictogram square that is lit when on and pale when off, with its
	// label beside it. As a radio option, lighting one puts out the others beside it.
	class Option : SignControl {
		public readonly bool Radio;
		public readonly string Icon;
		public Color Lit = Theme.Ink;   // the square's colour when on (the role options use the robot's colour)
		public event EventHandler CheckedChanged;
		bool on, hot;

		public Option(Look look, string text, string icon, bool radio) : base(look, true) {
			Text = AccessibleName = text;
			AccessibleRole = radio ? AccessibleRole.RadioButton : AccessibleRole.CheckButton;
			Icon = icon;
			Radio = radio;
			Cursor = Cursors.Hand;
			Size = new Size(P(3) + Square + P(7) + (int)Math.Ceiling(Ui.Width(text, look.Body)) + P(8), P(26));
		}

		int Square { get { return P(22); } }

		public bool Checked {
			get { return on; }
			set {
				if (on == value) return;
				on = value;
				if (on && Radio && Parent != null)
					foreach (Control c in Parent.Controls) { var o = c as Option; if (o != null && o != this && o.Radio) o.Checked = false; }
				Invalidate();
				if (CheckedChanged != null) CheckedChanged(this, EventArgs.Empty);
			}
		}

		// what a click or the space bar does
		public void Press() {
			if (!Enabled) return;
			if (Radio) Checked = true; else Checked = !Checked;
		}

		protected override void OnMouseEnter(EventArgs e) { hot = true; Invalidate(); base.OnMouseEnter(e); }
		protected override void OnMouseLeave(EventArgs e) { hot = false; Invalidate(); base.OnMouseLeave(e); }
		protected override void OnMouseDown(MouseEventArgs e) { if (e.Button == MouseButtons.Left) { Focus(); Press(); } base.OnMouseDown(e); }
		protected override bool IsInputKey(Keys key) {
			return (Radio && (key == Keys.Left || key == Keys.Right || key == Keys.Up || key == Keys.Down)) || base.IsInputKey(key);
		}
		protected override void OnKeyDown(KeyEventArgs e) {
			if (e.KeyCode == Keys.Space) { Press(); e.Handled = true; }
			else if (Radio && Parent != null && (e.KeyCode == Keys.Left || e.KeyCode == Keys.Up || e.KeyCode == Keys.Right || e.KeyCode == Keys.Down)) {
				// the arrow keys walk through the options of the group, as with radio buttons
				var group = new List<Option>();
				foreach (Control c in Parent.Controls) { var o = c as Option; if (o != null && o.Radio && o.Enabled) group.Add(o); }
				int i = group.IndexOf(this), step = e.KeyCode == Keys.Left || e.KeyCode == Keys.Up ? -1 : 1;
				if (i >= 0 && group.Count > 1) { var next = group[(i + step + group.Count) % group.Count]; next.Focus(); next.Press(); }
				e.Handled = true;
			}
			base.OnKeyDown(e);
		}

		protected override void OnPaint(PaintEventArgs e) {
			var g = e.Graphics;
			Ui.Prepare(g);
			g.Clear(BackColor);
			var all = new RectangleF(0, 0, Width, Height);
			if (hot && Enabled) Ui.FillRound(g, Theme.Mix(BackColor, Theme.Ink, 0.07f), all, P(4));
			var square = new RectangleF(P(3), (Height - Square) / 2f, Square, Square);
			Color fill = !Enabled ? Theme.Mix(Theme.Unlit, Theme.Panel, on ? 0 : 0.45f) : on ? Lit : Theme.Unlit;
			Color glyph = !Enabled ? (on ? Theme.Panel : Theme.Mix(Theme.Pale, Theme.Panel, 0.4f)) : on ? Theme.On(Lit) : Theme.Pale;
			Pict.Tile(g, Icon, square, fill, glyph);
			var text = new RectangleF(square.Right + P(7), 0, Width - square.Right - P(7), Height);
			Ui.Text(g, Text, L.Body, !Enabled ? Theme.Pale : on ? Theme.Ink : Theme.Grey, text);
			if (KeyFocus && Enabled) Ui.StrokeRound(g, Theme.Ink, all, P(4), Math.Max(1, P(1)));
		}
	}

	// A colour to pick, shown as a lit square with its name beside it: a click opens the colour dialog, a right
	// click (or Delete) goes back to the default, which is described in words.
	class ColorPick : SignControl {
		public Color? Chosen { get; set; }  // null: the default
		public string DefaultText = "";   // what the default is, shown in place of a colour
		public event EventHandler Changed;
		bool hot;

		public ColorPick(Look look, string text) : base(look, true) {
			Text = AccessibleName = text;
			AccessibleRole = AccessibleRole.PushButton;
			Cursor = Cursors.Hand;
			Size = new Size(P(3) + Square + P(7) + (int)Math.Ceiling(Ui.Width(text + ": robot colours", look.Body)) + P(8), P(26));
		}

		int Square { get { return P(22); } }

		void Set(Color? colour) {
			Chosen = colour;
			Invalidate();
			if (Changed != null) Changed(this, EventArgs.Empty);
		}

		public void Pick() {
			using (var dialog = new ColorDialog { FullOpen = true, AnyColor = true, Color = Chosen ?? Theme.Blue }) {
				if (dialog.ShowDialog(FindForm()) == DialogResult.OK) Set(dialog.Color);
			}
		}

		protected override void OnMouseEnter(EventArgs e) { hot = true; Invalidate(); base.OnMouseEnter(e); }
		protected override void OnMouseLeave(EventArgs e) { hot = false; Invalidate(); base.OnMouseLeave(e); }
		protected override void OnMouseDown(MouseEventArgs e) {
			Focus();
			if (e.Button == MouseButtons.Left) Pick();
			else if (e.Button == MouseButtons.Right) Set(null);
			base.OnMouseDown(e);
		}
		protected override void OnKeyDown(KeyEventArgs e) {
			if (e.KeyCode == Keys.Space || e.KeyCode == Keys.Enter) { Pick(); e.Handled = true; }
			else if (e.KeyCode == Keys.Delete || e.KeyCode == Keys.Back) { Set(null); e.Handled = true; }
			base.OnKeyDown(e);
		}
		protected override bool IsInputKey(Keys key) { return key == Keys.Enter || base.IsInputKey(key); }

		protected override void OnPaint(PaintEventArgs e) {
			var g = e.Graphics;
			Ui.Prepare(g);
			g.Clear(BackColor);
			var all = new RectangleF(0, 0, Width, Height);
			if (hot && Enabled) Ui.FillRound(g, Theme.Mix(BackColor, Theme.Ink, 0.07f), all, P(4));
			var square = new RectangleF(P(3), (Height - Square) / 2f, Square, Square);
			if (Chosen.HasValue) {
				Ui.FillRound(g, Enabled ? Chosen.Value : Theme.Mix(Chosen.Value, Theme.Panel, 0.5f), square, P(3));
			} else {
				// the default: the two robots' colours, halved
				var left = new RectangleF(square.X, square.Y, square.Width / 2, square.Height);
				var right = new RectangleF(square.X + square.Width / 2, square.Y, square.Width / 2, square.Height);
				g.SetClip(left);
				Ui.FillRound(g, Enabled ? Theme.Blue : Theme.Mix(Theme.Blue, Theme.Panel, 0.5f), square, P(3));
				g.SetClip(right);
				Ui.FillRound(g, Enabled ? Theme.Orange : Theme.Mix(Theme.Orange, Theme.Panel, 0.5f), square, P(3));
				g.ResetClip();
			}
			Ui.StrokeRound(g, Enabled ? Theme.Ink : Theme.Pale, square, P(3), Math.Max(1, P(1)));
			string words = Text + ": " + (Chosen.HasValue ? "#" + Chosen.Value.R.ToString("X2") + Chosen.Value.G.ToString("X2") + Chosen.Value.B.ToString("X2") : DefaultText);
			var text = new RectangleF(square.Right + P(7), 0, Width - square.Right - P(7), Height);
			Ui.Text(g, words, L.Body, Enabled ? Theme.Ink : Theme.Pale, text);
			if (KeyFocus && Enabled) Ui.StrokeRound(g, Theme.Ink, all, P(4), Math.Max(1, P(1)));
		}
	}

	// A framed text box: white, a thin black outline (thicker while typing in it), an optional pictogram.
	class TextField : SignControl {
		public readonly TextBox Box;
		readonly string icon, prompt;
		bool entered; // the player is in the box (it stays so while another window is in front)
		bool shown;   // the text box is showing, rather than the prompt

		public TextField(Look look, string icon, string prompt) : base(look, true) {
			this.icon = icon;
			this.prompt = prompt ?? "";
			Height = P(28);
			Cursor = Cursors.IBeam;
			Box = new TextBox { BorderStyle = BorderStyle.None, Font = look.Body, BackColor = Theme.Input, ForeColor = Theme.Ink };
			Box.Enter += (s, e) => { entered = true; Settle(); };
			Box.Leave += (s, e) => { entered = false; Settle(); };
			Box.GotFocus += (s, e) => Invalidate();
			Box.LostFocus += (s, e) => Invalidate();
			Box.TextChanged += (s, e) => Settle();
			Controls.Add(Box);
			Place();
			Settle();
		}

		// The prompt (pale words that say what to type) is drawn by the field itself, in its own colour: the
		// text box only shows while it has text or the player is in it. Until then the field takes the
		// keyboard focus and hands it on.
		void Settle() {
			shown = Box.Text.Length > 0 || entered;
			Box.Visible = shown;
			TabStop = !shown;
			Invalidate();
		}

		public void TakeFocus() {
			Box.Visible = true;
			Box.Focus();
			Box.SelectAll();
		}

		void Place() {
			int left = icon != null ? P(31) : P(9);
			Box.SetBounds(left, (Height - Box.PreferredHeight) / 2, Math.Max(10, Width - left - P(9)), Box.PreferredHeight);
		}

		protected override void OnResize(EventArgs e) { base.OnResize(e); if (Box != null) Place(); }
		protected override void OnMouseDown(MouseEventArgs e) { TakeFocus(); base.OnMouseDown(e); }
		protected override void OnGotFocus(EventArgs e) { base.OnGotFocus(e); TakeFocus(); }

		protected override void OnPaint(PaintEventArgs e) {
			var g = e.Graphics;
			Ui.Prepare(g);
			g.Clear(BackColor);
			var r = new RectangleF(0, 0, Width, Height);
			Ui.FillRound(g, Theme.Input, r, P(3));
			Ui.StrokeRound(g, Theme.Ink, r, P(3), Math.Max(1, P(1)) * (Box.Focused || AllFocusCues ? 2 : 1));
			if (icon != null) Pict.Draw(g, icon, new RectangleF(P(8), (Height - P(17)) / 2f, P(17), P(17)), Theme.Grey, Theme.Input);
			if (!shown) Ui.Text(g, prompt, L.Body, Theme.Pale, new RectangleF(Box.Left + P(1), 0, Box.Width, Height));
		}
	}

	// A number with a minus and a plus beside it. It can be typed, stepped with the buttons (held down, they
	// repeat), the up and down keys or the mouse wheel.
	class Stepper : SignControl {
		readonly double min, max, step;
		readonly int decimals;
		readonly TextBox box;
		readonly Timer repeat = new Timer();
		double value;
		int hot, held; // -1 the minus, +1 the plus, 0 neither
		public event EventHandler ValueChanged;

		public Stepper(Look look, double min, double max, double step, int decimals) : base(look, false) {
			this.min = min; this.max = max; this.step = step; this.decimals = decimals;
			value = min;
			Size = new Size(Side * 2 + P(50), P(26));
			box = new TextBox { BorderStyle = BorderStyle.None, Font = look.Strong, BackColor = Theme.Input, ForeColor = Theme.Ink, TextAlign = HorizontalAlignment.Center };
			box.SetBounds(Side + 1, (Height - box.PreferredHeight) / 2, Width - Side * 2 - 2, box.PreferredHeight);
			box.KeyDown += (s, e) => {
				if (e.KeyCode == Keys.Up) { Bump(1); e.Handled = true; }
				else if (e.KeyCode == Keys.Down) { Bump(-1); e.Handled = true; }
				else if (e.KeyCode == Keys.Enter) { Commit(); box.SelectAll(); e.Handled = e.SuppressKeyPress = true; }
			};
			box.Leave += (s, e) => Commit();
			box.GotFocus += (s, e) => Invalidate();
			box.LostFocus += (s, e) => Invalidate();
			box.MouseWheel += (s, e) => {
				Bump(Math.Sign(e.Delta));
				var handled = e as HandledMouseEventArgs;
				if (handled != null) handled.Handled = true;
			};
			Controls.Add(box);
			repeat.Tick += (s, e) => { repeat.Interval = 70; if (held != 0) Bump(held); };
			Display(value);
		}

		int Side { get { return P(26); } }

		// the same tooltip over the buttons and the number
		public void Tip(ToolTip tips, string text) { tips.SetToolTip(this, text); tips.SetToolTip(box, text); }

		public double Value {
			get { return value; }
			set {
				double v = Math.Round(Math.Max(min, Math.Min(max, value)), decimals);
				bool changed = v != this.value;
				this.value = v;
				Display(v);
				if (changed && ValueChanged != null) ValueChanged(this, EventArgs.Empty);
			}
		}

		void Display(double v) { box.Text = v.ToString("F" + decimals, CultureInfo.CurrentCulture); }

		// one step up or down (or none: just take what is typed in the box)
		public void Bump(int direction) {
			Commit();
			if (direction != 0) Value = value + direction * step;
		}

		// takes what was typed (with a point or a comma); nonsense puts the number back
		void Commit() {
			double typed;
			if (double.TryParse(box.Text.Trim().Replace(',', '.'), NumberStyles.Float, CultureInfo.InvariantCulture, out typed)) Value = typed;
			else Display(value);
		}

		int SideAt(int x) { return x < Side ? -1 : x >= Width - Side ? 1 : 0; }

		protected override void OnMouseMove(MouseEventArgs e) { int h = SideAt(e.X); if (h != hot) { hot = h; Invalidate(); } base.OnMouseMove(e); }
		protected override void OnMouseLeave(EventArgs e) { hot = 0; Invalidate(); base.OnMouseLeave(e); }
		protected override void OnMouseDown(MouseEventArgs e) {
			held = e.Button == MouseButtons.Left ? SideAt(e.X) : 0;
			if (held != 0) { Bump(held); repeat.Interval = 400; repeat.Start(); Invalidate(); }
			base.OnMouseDown(e);
		}
		protected override void OnMouseUp(MouseEventArgs e) { held = 0; repeat.Stop(); Invalidate(); base.OnMouseUp(e); }
		protected override void Dispose(bool disposing) { if (disposing) repeat.Dispose(); base.Dispose(disposing); }

		protected override void OnPaint(PaintEventArgs e) {
			var g = e.Graphics;
			Ui.Prepare(g);
			g.Clear(BackColor);
			var r = new RectangleF(0, 0, Width, Height);
			float line = Math.Max(1, P(1));
			Ui.FillRound(g, Theme.Input, r, P(3));
			for (int side = -1; side <= 1; side += 2) {
				var part = new RectangleF(side < 0 ? 0 : Width - Side, 0, Side, Height);
				bool pressed = held == side, over = hot == side && Enabled;
				if (pressed || over) {
					// (the outer corners stay round: fill the whole shape, clipped to this end)
					var clip = g.Clip;
					g.SetClip(part);
					Ui.FillRound(g, pressed ? Theme.Ink : Theme.Mix(Theme.Input, Theme.Ink, 0.12f), r, P(3));
					g.Clip = clip;
				}
				float mark = P(16);
				Pict.Draw(g, side < 0 ? "minus" : "plus", new RectangleF(part.X + (Side - mark) / 2, (Height - mark) / 2, mark, mark),
					!Enabled ? Theme.Pale : pressed ? Color.White : Theme.Ink, Theme.Input);
				Ui.Fill(g, Theme.Ink, new RectangleF(side < 0 ? Side - line : Width - Side, 0, line, Height));
			}
			Ui.StrokeRound(g, Theme.Ink, r, P(3), line * (box.Focused || AllFocusCues ? 2 : 1));
		}
	}

	// Progress as a strip of tick marks, filled from the left.
	class TickBar : SignControl {
		int value, maximum = 1;

		public TickBar(Look look) : base(look, false) { Size = new Size(P(120), P(12)); }

		public void Set(int done, int total) {
			total = Math.Max(1, total);
			done = Math.Max(0, Math.Min(done, total));
			if (done == value && total == maximum) return;
			value = done; maximum = total;
			Invalidate();
		}

		protected override void OnPaint(PaintEventArgs e) {
			var g = e.Graphics;
			g.Clear(BackColor);
			int tick = Math.Max(2, P(2)), pitch = tick * 2, count = Math.Max(1, (Width + tick) / pitch);
			int lit = (int)Math.Ceiling(count * (double)value / maximum);
			for (int i = 0; i < count; i++) Ui.Fill(g, i < lit ? Theme.Ink : Theme.Mix(Theme.Pale, BackColor, 0.35f), new RectangleF(i * pitch, 0, tick, Height));
		}
	}

	// Holds content that needs a least height. When there is less room than that (a low window), the content keeps
	// its height and scrolls, with the same slim bar the lists have; the mouse wheel works anywhere over it, and
	// a control that takes the keyboard focus is brought into view.
	class ScrollBox : SignControl {
		readonly Control content;
		int least, scroll, grab = -1;
		bool over;

		public ScrollBox(Look look, Control content) : base(look, false) {
			this.content = content;
			Controls.Add(content);
		}

		public int Least { get { return least; } set { if (least == value) return; least = value; Place(); } }

		int Gutter { get { return P(12); } }
		int Max { get { return Math.Max(0, least - Height); } }

		void Place() {
			scroll = Math.Max(0, Math.Min(scroll, Max));
			content.SetBounds(0, -scroll, Math.Max(0, Width - (Max > 0 ? Gutter : 0)), Math.Max(Height, least));
			Invalidate();
		}

		public void ScrollBy(int pixels) { scroll += pixels; Place(); }

		// brings a control inside into view
		public void Reveal(Control c) {
			int top = 0;
			for (var at = c; at != null && at != content; at = at.Parent) top += at.Top;
			if (top - P(4) < scroll) scroll = top - P(4);
			else if (top + c.Height + P(4) > scroll + Height) scroll = top + c.Height + P(4) - Height;
			Place();
		}

		// from now on, whatever inside takes the keyboard focus is brought into view
		public void FollowFocus() { Follow(content); }
		void Follow(Control parent) {
			foreach (Control c in parent.Controls) {
				var each = c;
				each.GotFocus += (s, e) => { if (Max > 0) Reveal(each); };
				Follow(each);
			}
		}

		Rectangle Thumb {
			get {
				if (Max == 0) return Rectangle.Empty;
				int length = Math.Max(P(30), (int)((long)Height * Height / least));
				return new Rectangle(Width - Gutter + P(5), (int)((long)(Height - length) * scroll / Max), Gutter - P(6), length);
			}
		}

		protected override void OnResize(EventArgs e) { base.OnResize(e); Place(); }

		protected override void OnMouseWheel(MouseEventArgs e) {
			if (Max > 0) {
				ScrollBy(-e.Delta * P(48) / 120);
				var handled = e as HandledMouseEventArgs;
				if (handled != null) handled.Handled = true;
			}
			base.OnMouseWheel(e);
		}

		protected override void OnMouseDown(MouseEventArgs e) {
			var thumb = Thumb;
			if (e.Button == MouseButtons.Left && !thumb.IsEmpty && e.X >= Width - Gutter) {
				if (e.Y >= thumb.Top && e.Y < thumb.Bottom) grab = e.Y - thumb.Top;
				else ScrollBy(e.Y < thumb.Top ? -Height : Height);
			}
			base.OnMouseDown(e);
		}

		protected override void OnMouseMove(MouseEventArgs e) {
			var thumb = Thumb;
			if (grab >= 0 && !thumb.IsEmpty && Height > thumb.Height) { scroll = (int)((long)(e.Y - grab) * Max / (Height - thumb.Height)); Place(); }
			else if (over != (!thumb.IsEmpty && e.X >= Width - Gutter)) { over = !over; Invalidate(); }
			base.OnMouseMove(e);
		}

		protected override void OnMouseUp(MouseEventArgs e) { grab = -1; Invalidate(); base.OnMouseUp(e); }
		protected override void OnMouseLeave(EventArgs e) { if (over) { over = false; Invalidate(); } base.OnMouseLeave(e); }

		protected override void OnPaint(PaintEventArgs e) {
			Ui.Prepare(e.Graphics);
			e.Graphics.Clear(BackColor);
			var thumb = Thumb;
			if (!thumb.IsEmpty) Ui.FillRound(e.Graphics, grab >= 0 ? Theme.Ink : over ? Theme.Grey : Theme.Pale, thumb, thumb.Width / 2f);
		}
	}

	// A list of rows drawn by a subclass: as many rows as you like (only those in view are drawn), one selected,
	// its own slim scroll bar. Mouse, wheel and keys (arrows, Page Up/Down, Home, End, Enter) all work.
	abstract class RowList : SignControl {
		protected int RowHeight, HeaderHeight;
		public event EventHandler SelectionChanged;  // the player picked a row
		public event EventHandler Activated;         // double-click or Enter on the selected row
		public Func<int, string> RowTip;             // what to say about a row in a tooltip
		int count, selected = -1, scroll, hot = -1, grab = -1;
		bool overThumb;
		string hint = "", tip;

		protected RowList(Look look) : base(look, true) {
			RowHeight = P(24);
			AccessibleRole = AccessibleRole.List;
		}

		public int Count {
			get { return count; }
			set { count = Math.Max(0, value); if (selected >= count) selected = -1; if (hot >= count) hot = -1; Clamp(); Invalidate(); }
		}

		public int Selected { get { return selected; } }

		// a line or two across the middle, for when there is nothing to list
		public string Hint { get { return hint; } set { if (hint == (value ?? "")) return; hint = value ?? ""; Invalidate(); } }

		// puts the selection on a row (-1: none) without that counting as the player's choice
		public void SelectRow(int row, bool reveal) {
			selected = row >= 0 && row < count ? row : -1;
			if (reveal) Reveal();
			Invalidate();
		}

		int Gutter { get { return P(12); } }                                   // room kept for the scroll bar
		protected int RowsWidth { get { return Math.Max(0, Width - Gutter); } }
		int View { get { return Math.Max(0, Height - HeaderHeight); } }
		int MaxScroll { get { return Math.Max(0, count * RowHeight - View); } }
		void Clamp() { scroll = Math.Max(0, Math.Min(scroll, MaxScroll)); }

		void Reveal() {
			if (selected < 0) return;
			int top = selected * RowHeight;
			if (top < scroll) scroll = top;
			else if (top + RowHeight > scroll + View) scroll = top + RowHeight - View;
			Clamp();
		}

		public void ScrollBy(int pixels) {
			int before = scroll;
			scroll += pixels;
			Clamp();
			if (scroll != before) Invalidate();
		}

		public int RowAt(Point p) {
			if (p.Y < HeaderHeight || p.X >= RowsWidth || p.X < 0) return -1;
			int row = (p.Y - HeaderHeight + scroll) / RowHeight;
			return row < count ? row : -1;
		}

		Rectangle Thumb {
			get {
				int max = MaxScroll;
				if (max == 0 || View < P(40)) return Rectangle.Empty;
				int length = Math.Max(P(30), (int)((long)View * View / ((long)count * RowHeight)));
				int at = (int)((long)(View - length) * scroll / max);
				return new Rectangle(Width - Gutter + P(3), HeaderHeight + at, Gutter - P(6), length);
			}
		}

		// ---- what the player does (also called by the self-test, which may not touch the real mouse or keyboard) ----

		public void Pick(int row) {
			if (row < 0 || row >= count || row == selected) return;
			selected = row;
			Reveal();
			Invalidate();
			if (SelectionChanged != null) SelectionChanged(this, EventArgs.Empty);
		}

		public void Activate() {
			if (selected >= 0 && Activated != null) Activated(this, EventArgs.Empty);
		}

		public bool Key(Keys key) {
			int page = Math.Max(1, View / RowHeight - 1);
			switch (key) {
				case Keys.Up: Step(-1); return true;
				case Keys.Down: Step(1); return true;
				case Keys.PageUp: Step(-page); return true;
				case Keys.PageDown: Step(page); return true;
				case Keys.Home: Pick(0); return true;
				case Keys.End: Pick(count - 1); return true;
				case Keys.Enter: Activate(); return true;
			}
			return false;
		}

		void Step(int by) {
			if (count == 0) return;
			Pick(selected < 0 ? (by > 0 ? 0 : count - 1) : Math.Max(0, Math.Min(count - 1, selected + by)));
		}

		public void ClickAt(Point p) {
			if (p.Y < HeaderHeight) HeaderClick(p.X);
			else Pick(RowAt(p));
		}

		protected virtual void HeaderClick(int x) { }

		// ---- input ----

		protected override bool IsInputKey(Keys key) {
			switch (key) {
				case Keys.Up: case Keys.Down: case Keys.PageUp: case Keys.PageDown: case Keys.Home: case Keys.End: case Keys.Enter: return true;
			}
			return base.IsInputKey(key);
		}

		protected override void OnKeyDown(KeyEventArgs e) {
			if (Key(e.KeyCode)) e.Handled = true;
			base.OnKeyDown(e);
		}

		protected override void OnMouseDown(MouseEventArgs e) {
			Focus();
			if (e.Button == MouseButtons.Left) {
				var thumb = Thumb;
				if (!thumb.IsEmpty && e.X >= Width - Gutter && e.Y >= HeaderHeight) {
					if (e.Y >= thumb.Top && e.Y < thumb.Bottom) grab = e.Y - thumb.Top;     // drag the bar
					else ScrollBy(e.Y < thumb.Top ? -View : View);                          // or page towards the click
				} else ClickAt(e.Location);
			}
			base.OnMouseDown(e);
		}

		protected override void OnMouseMove(MouseEventArgs e) {
			var thumb = Thumb;
			if (grab >= 0 && !thumb.IsEmpty) {
				int room = View - thumb.Height;
				if (room > 0) { scroll = (int)((long)(e.Y - HeaderHeight - grab) * MaxScroll / room); Clamp(); Invalidate(); }
			} else {
				bool over = !thumb.IsEmpty && e.X >= Width - Gutter;
				int row = over ? -1 : RowAt(e.Location);
				if (row != hot || over != overThumb) { hot = row; overThumb = over; Invalidate(); }
				string say = row >= 0 && RowTip != null ? RowTip(row) : null;
				if (say != tip && L.Tips != null) { tip = say; L.Tips.SetToolTip(this, say); }
			}
			base.OnMouseMove(e);
		}

		protected override void OnMouseUp(MouseEventArgs e) { grab = -1; Invalidate(); base.OnMouseUp(e); }
		protected override void OnMouseLeave(EventArgs e) { if (hot >= 0 || overThumb) { hot = -1; overThumb = false; Invalidate(); } base.OnMouseLeave(e); }
		protected override void OnMouseDoubleClick(MouseEventArgs e) { if (e.Button == MouseButtons.Left && RowAt(e.Location) >= 0) Activate(); base.OnMouseDoubleClick(e); }
		protected override void OnMouseWheel(MouseEventArgs e) {
			ScrollBy(-e.Delta * 3 * RowHeight / 120);
			// (a list that scrolls keeps the wheel to itself; one with nothing to scroll lets whatever holds it scroll)
			var handled = e as HandledMouseEventArgs;
			if (handled != null && MaxScroll > 0) handled.Handled = true;
			base.OnMouseWheel(e);
		}
		protected override void OnResize(EventArgs e) { Clamp(); base.OnResize(e); }

		// ---- drawing ----

		// (the box is the whole row; what is written in it should stay left of RowsWidth, clear of the scroll bar)
		protected abstract void DrawRow(Graphics g, int row, Rectangle box, bool selected, bool hot);
		protected virtual void DrawHeader(Graphics g, Rectangle box) { }

		protected override void OnPaint(PaintEventArgs e) {
			var g = e.Graphics;
			Ui.Prepare(g);
			g.Clear(BackColor);
			var clip = g.Clip;
			g.SetClip(new Rectangle(0, HeaderHeight, Width, View), CombineMode.Intersect);
			int first = scroll / RowHeight;
			for (int row = first, y = HeaderHeight + first * RowHeight - scroll; row < count && y < Height; row++, y += RowHeight)
				DrawRow(g, row, new Rectangle(0, y, Width, RowHeight), row == selected, row == hot);
			if (count == 0 && hint.Length > 0)
				Ui.Text(g, hint, L.Body, Theme.Grey, new RectangleF(P(20), HeaderHeight + P(26), Width - P(40), Math.Max(0, View - P(26))), ContentAlignment.TopCenter, true);
			var thumb = Thumb;
			if (!thumb.IsEmpty) Ui.FillRound(g, grab >= 0 ? Theme.Ink : overThumb ? Theme.Grey : Theme.Pale, thumb, thumb.Width / 2f);
			g.Clip = clip;
			if (HeaderHeight > 0) DrawHeader(g, new Rectangle(0, 0, Width, HeaderHeight));
		}
	}

	// The demos: Map, Time, Players, Mode, File. Capital column heads over a rule, faint stripes, and the selected
	// row inverted like a lit sign with a marker in the portal colour.
	class DemoList : RowList {
		public static readonly string[] Titles = { "MAP", "TIME", "PLAYERS", "MODE", "FILE" };
		const int MapWidth = 150, TimeWidth = 66, PlayersWidth = 200, ModeWidth = 94, FileWidth = 160; // at 100%, in a roomy window
		readonly int[] widths = new int[5];
		public Func<int, Run> Source;
		public int SortColumn;
		public bool SortDescending;
		public event Action<int> HeaderClicked;

		public DemoList(Look look) : base(look) {
			RowHeight = P(25);
			HeaderHeight = P(30);
			Share();
		}

		protected override void OnResize(EventArgs e) { Share(); base.OnResize(e); }

		// The columns share the width: Map and Players give way in a narrow window and grow a little in a wide
		// one; File takes what is left.
		void Share() {
			int width = RowsWidth;
			double f = Math.Max(0.5, Math.Min(1.25, (width - P(TimeWidth + ModeWidth)) / (double)P(MapWidth + PlayersWidth + FileWidth)));
			widths[0] = (int)(P(MapWidth) * f);
			widths[1] = P(TimeWidth);
			widths[2] = (int)(P(PlayersWidth) * f);
			widths[3] = P(ModeWidth);
			widths[4] = Math.Max(P(40), width - widths[0] - widths[1] - widths[2] - widths[3]);
		}

		// a column's cell in a row: where its text goes
		RectangleF Cell(Rectangle row, int column) {
			int x = row.X;
			for (int i = 0; i < column; i++) x += widths[i];
			float left = column == 0 ? P(14) : P(8);
			return new RectangleF(x + left, row.Y, widths[column] - left - P(column == 1 ? 10 : 6), row.Height);
		}

		protected override void HeaderClick(int x) {
			for (int i = 0, right = 0; i < widths.Length; i++) {
				right += widths[i];
				if (x < right) { if (HeaderClicked != null) HeaderClicked(i); return; }
			}
		}

		protected override void DrawHeader(Graphics g, Rectangle box) {
			Ui.Fill(g, BackColor, box);
			float tracking = L.S * 0.9f;
			for (int i = 0; i < Titles.Length; i++) {
				var cell = Cell(box, i);
				bool sorted = i == SortColumn, right = i == 1; // (Time is set to the right, like its numbers)
				float arrow = P(7);
				if (sorted && right) cell.Width -= arrow + P(5);
				float end = Ui.Tracked(g, Titles[i], L.Caps, sorted ? Theme.Ink : Theme.Grey, cell, tracking, right ? ContentAlignment.MiddleRight : ContentAlignment.MiddleLeft);
				if (!sorted) continue;
				// a small triangle after the column the list is sorted by: pointing up for ascending
				float x = end + P(5), y = box.Y + box.Height / 2f, h = arrow * 0.62f;
				var points = SortDescending ? new[] { new PointF(x, y - h / 2), new PointF(x + arrow, y - h / 2), new PointF(x + arrow / 2, y + h / 2) }
				                            : new[] { new PointF(x, y + h / 2), new PointF(x + arrow, y + h / 2), new PointF(x + arrow / 2, y - h / 2) };
				using (var b = new SolidBrush(Theme.Ink)) g.FillPolygon(b, points);
			}
			Ui.Fill(g, Theme.Ink, new RectangleF(0, box.Bottom - Math.Max(1, P(1)), box.Width, Math.Max(1, P(1))));
		}

		protected override void DrawRow(Graphics g, int row, Rectangle box, bool selected, bool hot) {
			var run = Source != null ? Source(row) : null;
			if (run == null) return;
			Color back = selected ? Theme.Ink : hot ? Theme.Mix(Theme.Panel, Theme.Ink, 0.09f) : row % 2 == 1 ? Theme.Stripe : BackColor;
			Ui.Fill(g, back, box);
			Color ink = selected ? Color.White : run.Ok ? Theme.Ink : Theme.Pale;
			Color soft = selected ? Theme.Mix(Color.White, Theme.Ink, 0.22f) : run.Ok ? Theme.Grey : Theme.Pale;
			if (selected) {
				// the marker: the portal colour of the robot about to be played
				Ui.Fill(g, L.Accent, new RectangleF(box.X, box.Y, P(5), box.Height));
				if (KeyFocus) Ui.StrokeRound(g, Color.FromArgb(150, Color.White), new RectangleF(box.X + P(7), box.Y + P(2), box.Width - P(9), box.Height - P(4)), 0, Math.Max(1, P(1)));
			}
			Ui.Text(g, run.MapName, L.Body, ink, Cell(box, 0));
			Ui.Text(g, run.TimeText, L.Body, ink, Cell(box, 1), ContentAlignment.MiddleRight);
			Ui.Text(g, run.Players, L.Body, ink, Cell(box, 2));
			Ui.Text(g, run.Files, L.Body, soft, Cell(box, 4));

			// mode: SP or CO-OP, and a pip for each robot whose demo is there
			var mode = Cell(box, 3);
			if (run.Ok) {
				string word = run.Coop ? "CO-OP" : "SP";
				Ui.Text(g, word, L.Body, soft, mode);
				if (run.Coop) {
					float pip = P(7), x = mode.X + Ui.Width(word, L.Body) + P(7), y = box.Y + (box.Height - pip) / 2f;
					bool blue = run.Partner != null || run.Demo.Scan.Host, orange = run.Partner != null || !run.Demo.Scan.Host;
					if (blue) { Ui.Fill(g, Theme.Blue, new RectangleF(x, y, pip, pip)); x += pip + P(3); }
					if (orange) Ui.Fill(g, Theme.Orange, new RectangleF(x, y, pip, pip));
				}
			}
		}
	}

	// The run's parts, like the row of pictograms on a chamber sign: a numbered square, the part's name, its
	// time span. The selected row is where the player starts; the parts before it are dimmed (the record plays
	// those), the ones from it on are lit.
	class PartsList : RowList {
		List<RoutePart> parts = new List<RoutePart>();

		public PartsList(Look look) : base(look) { RowHeight = P(31); }

		public void SetParts(List<RoutePart> list, int startRow) {
			parts = list ?? new List<RoutePart>();
			Count = parts.Count;
			SelectRow(startRow, true);
		}

		public int Part(int row) { return row >= 0 && row < parts.Count ? parts[row].Index : 1; }
		public int RowOf(int part) { return parts.FindIndex(p => p.Index == part); }
		public string Describe(int row) {
			return row >= 0 && row < parts.Count ? parts[row].Index + ". " + parts[row].Name + "\n" + RouteCli.FormatTime(parts[row].Start) + " – " + RouteCli.FormatTime(parts[row].End) : null;
		}

		protected override void DrawRow(Graphics g, int row, Rectangle box, bool selected, bool hot) {
			var part = parts[row];
			int start = Selected;
			bool before = start >= 0 && row < start;
			Ui.Fill(g, selected ? Theme.Input : hot ? Theme.Mix(BackColor, Theme.Ink, 0.07f) : BackColor, box);
			if (selected) {
				Ui.Fill(g, L.Accent, new RectangleF(box.X, box.Y, P(4), box.Height));
				float line = Math.Max(1, P(1));
				Ui.Fill(g, Theme.Line, new RectangleF(box.X, box.Y, box.Width, line));
				Ui.Fill(g, Theme.Line, new RectangleF(box.X, box.Bottom - line, box.Width, line));
			}
			float size = P(23), x = box.X + P(12), y = box.Y + (box.Height - size) / 2f, rail = Math.Max(2, P(2));
			// the rail that joins the squares: pale up to the start, black from there
			if (row > 0) Ui.Fill(g, row <= start ? Theme.Unlit : Theme.Ink, new RectangleF(x + (size - rail) / 2, box.Y, rail, y - box.Y));
			if (row < parts.Count - 1) Ui.Fill(g, row < start ? Theme.Unlit : Theme.Ink, new RectangleF(x + (size - rail) / 2, y + size, rail, box.Bottom - y - size));
			Color fill = before ? Theme.Unlit : selected ? L.Accent : Theme.Ink;
			var square = new RectangleF(x, y, size, size);
			Ui.FillRound(g, fill, square, size * 0.17f);
			Ui.Text(g, part.Index.ToString("00"), L.Strong, before ? Theme.Pale : Theme.On(fill), square, ContentAlignment.MiddleCenter);

			string span = RouteCli.FormatTime(part.Start) + " – " + RouteCli.FormatTime(part.End);
			float spanWidth = Ui.Width(span, L.Small) + P(4), left = square.Right + P(10), right = RowsWidth - P(2);
			Ui.Text(g, span, L.Small, before ? Theme.Pale : Theme.Grey, new RectangleF(right - spanWidth, box.Y, spanWidth, box.Height), ContentAlignment.MiddleRight);
			Ui.Text(g, part.Name, L.Body, before ? Theme.Pale : Theme.Ink, new RectangleF(left, box.Y, right - spanWidth - P(10) - left, box.Height));
			if (selected && KeyFocus) Ui.StrokeRound(g, Theme.Ink, new RectangleF(box.X + P(6), box.Y + P(2), box.Width - P(8), box.Height - P(4)), 0, Math.Max(1, P(1)));
		}
	}

	// The selected run, laid out as a chamber sign lays out its number: the time in huge numerals, the map's
	// name in capitals, the players, and a strip of tick marks for the parts with the part the player starts
	// from lit in the portal colour.
	class RunHeader : SignControl {
		public string Time = "", Title = "", Sub = "", Problem;
		public bool Empty = true;                                  // no run: everything pale
		public class Player { public Color Chip; public string Name; public bool Aside; }  // Chip: the robot's colour (none in single player); Aside: a remark, not a name
		public readonly List<Player> Players = new List<Player>();
		public List<RoutePart> Parts = new List<RoutePart>();
		public int Start = -1;                                     // the row of the part the player starts from

		public RunHeader(Look look) : base(look, false) { Height = look.P(122); }

		protected override void OnPaint(PaintEventArgs e) {
			var g = e.Graphics;
			Ui.Prepare(g);
			g.Clear(BackColor);
			float w = Width;

			// the numerals, top left, as tall as the sign allows; a long time (minutes) in a smaller size
			string time = Time.Length > 0 ? Time : "--";
			var face = Ui.Width(time, L.Huge) <= w * 0.44f ? L.Huge : L.Large;
			float timeWidth = Ui.Width(time, face);
			Ui.Text(g, time, face, Empty ? Theme.Unlit : Theme.Ink, new RectangleF(-P(2), -P(8), timeWidth + P(8), P(80)), ContentAlignment.BottomLeft);

			// beside them: the map in capitals, what it is underneath
			float x = timeWidth + P(18);
			Ui.Text(g, Title, L.Title, Empty && Problem == null ? Theme.Grey : Theme.Ink, new RectangleF(x, P(10), w - x, P(32)), ContentAlignment.MiddleLeft);
			Ui.Tracked(g, Sub, L.Caps, Theme.Grey, new RectangleF(x, P(42), w - x, P(20)), L.S * 0.9f);

			// who: a chip in the robot's colour before each name
			float px = 0, py = P(75), chip = P(9);
			if (Problem != null) Ui.Text(g, Problem, L.Body, Theme.Alert, new RectangleF(0, py, w, P(20)));
			else foreach (var player in Players) {
				if (!player.Chip.IsEmpty) { Ui.Fill(g, player.Chip, new RectangleF(px, py + (P(20) - chip) / 2f, chip, chip)); px += chip + P(6); }
				float room = w - px, name = Math.Min(room, Ui.Width(player.Name, L.Body) + P(2));
				Ui.Text(g, player.Name, L.Body, player.Aside ? Theme.Grey : Theme.Ink, new RectangleF(px, py, name, P(20)));
				px += name + P(18);
				if (px >= w) break;
			}

			// the parts: "start part / parts", then tick marks - a tall one where each part begins
			float top = P(102), height = P(18);
			string count = Parts.Count > 0 && Start >= 0 ? (Start + 1).ToString("00") + "/" + Parts.Count.ToString("00") : "--/--";
			Ui.Text(g, count, L.Strong, Parts.Count > 0 ? Theme.Ink : Theme.Pale, new RectangleF(0, top, P(60), height), ContentAlignment.BottomLeft);
			float from = Ui.Width("00/00", L.Strong) + P(12), tick = Math.Max(2, P(3)), pitch = tick + Math.Max(2, P(2));
			int ticks = Math.Max(1, (int)((w - from + (pitch - tick)) / pitch));
			double total = 0;
			foreach (var p in Parts) total += Math.Max(0, p.End - p.Start);
			int part = 0, lastPart = -1;
			double passed = 0;
			for (int i = 0; i < ticks; i++) {
				Color color = Theme.Unlit;
				bool tall = false;
				if (Parts.Count > 0 && total > 0) {
					// the part this tick falls in, by time
					double at = total * i / ticks;
					while (part < Parts.Count - 1 && at >= passed + Math.Max(0, Parts[part].End - Parts[part].Start)) { passed += Math.Max(0, Parts[part].End - Parts[part].Start); part++; }
					tall = part != lastPart;
					lastPart = part;
					color = part < Start ? Theme.Mix(Theme.Pale, Theme.Unlit, 0.4f) : part == Start ? L.Accent : Theme.Ink;
				}
				float h = tall ? height : height * 0.55f;
				Ui.Fill(g, color, new RectangleF((float)Math.Round(from + i * pitch), top + height - h, tick, h));
			}
		}
	}

	// The menus, in the same style: white capitals on the dark band, drop-downs as off-white boards whose
	// highlighted item is inverted.
	class SignMenuRenderer : ToolStripProfessionalRenderer {
		public SignMenuRenderer() { RoundedEdges = false; }

		static bool Top(ToolStripItem item) { return item.Owner is MenuStrip; }

		protected override void OnRenderToolStripBackground(ToolStripRenderEventArgs e) {
			using (var b = new SolidBrush(e.ToolStrip is MenuStrip ? Theme.Dark : Theme.Panel)) e.Graphics.FillRectangle(b, e.AffectedBounds);
		}

		protected override void OnRenderToolStripBorder(ToolStripRenderEventArgs e) {
			if (e.ToolStrip is MenuStrip) return;
			using (var pen = new Pen(Theme.Ink)) e.Graphics.DrawRectangle(pen, 0, 0, e.ToolStrip.Width - 1, e.ToolStrip.Height - 1);
		}

		protected override void OnRenderImageMargin(ToolStripRenderEventArgs e) { }

		protected override void OnRenderMenuItemBackground(ToolStripItemRenderEventArgs e) {
			var item = e.Item as ToolStripMenuItem;
			bool lit = item != null && item.Enabled && (item.Selected || item.Pressed);
			var box = new Rectangle(Point.Empty, e.Item.Size);
			Color fill = Top(e.Item) ? (lit ? Theme.Mix(Theme.Dark, Color.White, 0.18f) : Theme.Dark) : (lit ? Theme.Ink : Theme.Panel);
			if (!Top(e.Item)) box.Inflate(-2, 0);
			using (var b = new SolidBrush(fill)) e.Graphics.FillRectangle(b, box);
		}

		protected override void OnRenderItemText(ToolStripItemTextRenderEventArgs e) {
			bool lit = e.Item.Enabled && (e.Item.Selected || e.Item.Pressed);
			e.TextColor = Top(e.Item) ? Color.White : !e.Item.Enabled ? Theme.Pale : lit ? Color.White : Theme.Ink;
			base.OnRenderItemText(e);
		}

		protected override void OnRenderArrow(ToolStripArrowRenderEventArgs e) {
			e.ArrowColor = e.Item.Enabled && (e.Item.Selected || e.Item.Pressed) ? Color.White : Theme.Ink;
			base.OnRenderArrow(e);
		}

		protected override void OnRenderSeparator(ToolStripSeparatorRenderEventArgs e) {
			using (var pen = new Pen(Theme.Line)) e.Graphics.DrawLine(pen, 8, e.Item.Height / 2, e.Item.Width - 8, e.Item.Height / 2);
		}
	}

	static class Native {
		[StructLayout(LayoutKind.Sequential)]
		public struct RECT { public int Left, Top, Right, Bottom; }

		[DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr window);

		// pixels per 100%-scale unit on the monitor the window is on (1 = 100%, 1.5 = 150%)
		public static float ScaleOf(Control window) {
			try {
				if (window != null && window.IsHandleCreated) {
					uint dpi = GetDpiForWindow(window.Handle);
					if (dpi > 0) return dpi / 96f;
				}
			} catch (EntryPointNotFoundException) { } // before Windows 10 1607
			using (var g = Graphics.FromHwnd(IntPtr.Zero)) return g.DpiX / 96f;
		}
	}
}
