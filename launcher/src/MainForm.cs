// The window: a dark band along the top (the mark, the menus), the demos on a board on the left, and on the
// right the selected run as a chamber sign - its time in huge numerals, its parts, the options as pictograms -
// with Launch at the bottom. A status line runs along the foot. How it all looks is in Theme.cs and Controls.cs.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;
using Timer = System.Windows.Forms.Timer;

namespace P2Practice {
	class MainForm : Form {
		// sizes at 100% scale
		const int DefaultWidth = 1260, DefaultHeight = 880, MinWidth = 1010, MinHeight = 730, SideWidth = 474;

		readonly Settings settings;
		readonly DemoLibrary library = new DemoLibrary();
		readonly Timer poll = new Timer { Interval = 150 };  // picks up what the library read in the background
		readonly Dictionary<string, RouteInfo> infos = new Dictionary<string, RouteInfo>(); // run key -> its parts, once read

		// The window is laid out by hand for one scale: every size goes through P() and every font comes from the
		// Look made for that scale. When the window moves to a monitor with another scale it is built again (Rescale).
		float S = 1;
		int P(float v) { return (int)Math.Round(v * S); }
		Look look;
		ToolTip tips;

		MenuStrip menu;
		ToolStripMenuItem demosMenu, gameMenu;
		Panel band;
		TextField search, extra;
		DemoList list;
		PartsList parts;
		RunHeader sign;
		Words notes, status, libraryLabel;
		Caption roleCaption, partnerCaption;
		Option practice, watch, blue, orange, bot, real, ghost, clock, keys, line, loop;
		ColorPick ghostColor;
		Stepper ghostOpacity;
		Stepper leadIn, speed;
		SignButton launch;
		TickBar progress;

		List<Run> allRuns = new List<Run>(), rows = new List<Run>();
		Run selected;
		string searchText = "";
		string partRun;        // the run the player chose a part of...
		int partChosen = 1;    // ...and the part
		int shownVersion = -1;
		DateTime lastRefresh;
		bool building, infoBusy, progressShown;
		string message = "";
		bool messageIsError;

		public MainForm() {
			settings = Settings.Load();
			searchText = App.Search ?? "";
			Text = App.Title + (App.DryRun ? " (dry run)" : "");
			Icon = Program.AppIcon;
			AutoScaleMode = AutoScaleMode.None;
			StartPosition = FormStartPosition.Manual;
			AllowDrop = true;
			S = App.Dpi > 0 ? App.Dpi / 100f : Native.ScaleOf(null);
			BuildUi();
			PlaceWindow();

			if (!GameLocator.IsGameDir(settings.GameDir)) Say("Portal 2 wasn't found. Choose its folder under Game > Choose Portal 2 folder.", true);
			else if (!RouteCli.Exists) Say(RouteCli.MissingMessage + ".", true);
			else if (App.DryRun) Say("Dry run: Launch starts nothing and writes what it would do to " + App.LogPath);

			poll.Tick += (s, e) => Poll();
			DragEnter += (s, e) => e.Effect = e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Link : DragDropEffects.None;
			DragDrop += (s, e) => AddFolders(DroppedFolders((string[])e.Data.GetData(DataFormats.FileDrop)));
			library.Start(settings.Folders);
			poll.Start();
		}

		// ---- building the window ----

		void BuildUi() {
			building = true;
			SuspendLayout();
			look = new Look(S);
			look.Tips = tips = NewTips();
			Font = look.Body;
			BackColor = Theme.Field;
			ForeColor = Theme.Ink;

			var body = new Panel { Dock = DockStyle.Fill, BackColor = Theme.Field, Padding = new Padding(P(12), P(12), P(12), P(2)) };
			var board = BuildLibrary();
			var gap = new Panel { Dock = DockStyle.Right, Width = P(12), BackColor = Theme.Field };
			var side = BuildSide();
			// (docked from the last one added to the first: the sign on the right, the gap, and the list gets the rest)
			body.Controls.AddRange(new Control[] { board, gap, side });
			Controls.AddRange(new Control[] { body, BuildStatusBar(), BuildBand() });
			MainMenuStrip = menu;
			building = false;
			ResumeLayout(true);

			ShowOptions();
			ApplyRows(true);
			ShowRun(selected);
			ShowStatus();
		}

		// tooltips in the sign's colours: white on black
		ToolTip NewTips() {
			var t = new ToolTip { AutoPopDelay = 15000, InitialDelay = 500, ReshowDelay = 100, OwnerDraw = true };
			var face = look.Small;
			int pad = P(7), widest = P(420);
			t.Popup += (s, e) => {
				var size = Ui.Wrapped(t.GetToolTip(e.AssociatedControl), face, widest);
				e.ToolTipSize = new Size((int)Math.Ceiling(size.Width) + pad * 2 + 2, (int)Math.Ceiling(size.Height) + pad * 2);
			};
			t.Draw += (s, e) => {
				Ui.Prepare(e.Graphics);
				e.Graphics.Clear(Theme.Ink);
				Ui.Text(e.Graphics, e.ToolTipText, face, Color.White, new RectangleF(pad, pad, e.Bounds.Width - pad * 2, e.Bounds.Height - pad * 2), ContentAlignment.TopLeft, true);
			};
			return t;
		}

		// The dark band: the launcher's mark and name, then the menus.
		Control BuildBand() {
			const string name = "P2 PRACTICE";
			float tracking = S * 2.4f, mark = P(24);
			float nameAt = P(16) + mark + P(10), nameWidth = Ui.TrackedWidth(name, look.Brand, tracking);

			menu = new MenuStrip { Dock = DockStyle.None, AutoSize = true, BackColor = Theme.Dark, ForeColor = Color.White, Font = look.Strong,
				Renderer = new SignMenuRenderer(), Padding = Padding.Empty, GripStyle = ToolStripGripStyle.Hidden, CanOverflow = false, Stretch = false };
			demosMenu = new ToolStripMenuItem("&DEMOS") { Padding = new Padding(P(5), P(3), P(5), P(3)) };
			gameMenu = new ToolStripMenuItem("&GAME") { Padding = new Padding(P(5), P(3), P(5), P(3)) };
			// (filled again whenever they open: the folders and the game folder change)
			demosMenu.DropDownOpening += (s, e) => FillDemosMenu();
			gameMenu.DropDownOpening += (s, e) => FillGameMenu();
			menu.Items.Add(demosMenu);
			menu.Items.Add(gameMenu);
			FillDemosMenu();
			FillGameMenu();

			band = new Panel { Dock = DockStyle.Top, Height = P(44), BackColor = Theme.Dark };
			menu.Location = new Point((int)(nameAt + nameWidth) + P(26), (band.Height - menu.PreferredSize.Height) / 2);
			band.Controls.Add(menu);
			band.Paint += (s, e) => {
				var g = e.Graphics;
				Ui.Prepare(g);
				Pict.Draw(g, "iris", new RectangleF(P(16), (band.Height - mark) / 2f, mark, mark), Color.White, Theme.Dark);
				Ui.Tracked(g, name, look.Brand, Color.White, new RectangleF(nameAt, 0, nameWidth + 8, band.Height), tracking);
				Ui.Fill(g, Theme.Mix(Theme.Dark, Color.White, 0.28f), new RectangleF(nameAt + nameWidth + P(14), P(14), Math.Max(1, P(1)), band.Height - P(28)));
				if (App.DryRun) {
					// a tag, so nobody waits for a game that a dry run never starts
					const string tag = "DRY RUN";
					float w = Ui.TrackedWidth(tag, look.Caps, S) + P(16), h = P(20);
					var box = new RectangleF(band.Width - w - P(16), (band.Height - h) / 2f, w, h);
					Ui.StrokeRound(g, Color.White, box, P(3), Math.Max(1, P(1)));
					Ui.Tracked(g, tag, look.Caps, Color.White, box, S, ContentAlignment.MiddleCenter);
				}
			};
			band.Resize += (s, e) => band.Invalidate();
			return band;
		}

		// The board on the left: the search box, Add folder, and the list of runs.
		Control BuildLibrary() {
			search = new TextField(look, "search", "Search map, player or file");
			search.Box.Text = searchText;
			search.Box.TextChanged += (s, e) => { if (building) return; searchText = search.Box.Text; ApplyRows(); };
			var add = new SignButton(look, "ADD FOLDER") { Icon = "plus" };
			add.Width += P(12);
			add.Click += (s, e) => AddFolder();
			tips.SetToolTip(add, "Add a folder of demos (its subfolders are searched too). You can also drop a folder on the window.");

			var top = new Panel { Dock = DockStyle.Top, Height = P(54), BackColor = Theme.Panel };
			top.Controls.Add(search);
			top.Controls.Add(add);
			EventHandler place = (s, e) => {
				add.Location = new Point(top.Width - add.Width - P(13), P(13));
				search.SetBounds(P(13), P(13), Math.Max(P(60), add.Left - P(10) - P(13)), P(28));
			};
			top.Resize += place;

			list = new DemoList(look) { Dock = DockStyle.Fill, SortColumn = settings.SortColumn, SortDescending = settings.SortDescending != 0 };
			list.Source = i => i >= 0 && i < rows.Count ? rows[i] : null;
			list.RowTip = i => {
				var r = i >= 0 && i < rows.Count ? rows[i] : null;
				return r == null ? null : r.Demo.Path + (r.Partner != null ? "\n" + r.Partner.Path : "") + (r.Ok ? "" : "\n" + r.Demo.Scan.Error);
			};
			list.SelectionChanged += (s, e) => SyncSelection();
			list.Activated += (s, e) => Launch(); // double-click or Enter
			list.HeaderClicked += column => {
				if (settings.SortColumn == column) settings.SortDescending ^= 1;
				else { settings.SortColumn = column; settings.SortDescending = 0; }
				ApplyRows(true);
			};

			var card = new Card { Dock = DockStyle.Fill, Padding = new Padding(1) };
			card.Controls.Add(list);
			card.Controls.Add(top);
			return card;
		}

		// The sign on the right: the run, its parts, the options, Launch.
		Control BuildSide() {
			sign = new RunHeader(look) { Anchor = AnchorStyles.Left | AnchorStyles.Right, Margin = Padding.Empty };
			var partsCaption = new Caption(look, "START FROM") { Margin = Padding.Empty, Height = P(20) };

			parts = new PartsList(look) { Dock = DockStyle.Fill, Margin = Padding.Empty };
			parts.RowTip = i => parts.Describe(i);
			parts.SelectionChanged += (s, e) => {
				if (selected == null) return;
				partRun = selected.Key;
				partChosen = parts.Part(parts.Selected);
				sign.Start = parts.Selected;
				sign.Invalidate();
			};
			parts.Activated += (s, e) => Launch();

			notes = new Words(look, "", look.Small, Theme.Grey) { Anchor = AnchorStyles.Left | AnchorStyles.Right, Margin = new Padding(0, P(5), 0, 0), Visible = false,
				Wrap = true, Align = ContentAlignment.TopLeft, Height = look.Small.Height * 2 + P(2) }; // (room for two lines; the rest is in the tooltip)

			// the options: captions in a column, pictogram choices beside them
			var opt = new TableLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Dock = DockStyle.Fill, ColumnCount = 2, Margin = Padding.Empty, BackColor = Theme.Panel };
			opt.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, P(116)));
			opt.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));

			practice = Choice("Practice", "play", true, "Play it yourself: the record runs up to the part you chose, then you take over.");
			watch = Choice("Watch the run", "eye", true, "Ride along with the recorded run from the part you chose.");
			AddOption(opt, new Caption(look, "MODE"), Across(practice, watch));

			blue = Choice("Blue (ATLAS)", "atlas", true, null);
			blue.Lit = Theme.Blue;
			orange = Choice("Orange (P-body)", "pbody", true, null);
			orange.Lit = Theme.Orange;
			AddOption(opt, roleCaption = new Caption(look, "PLAY AS"), Across(blue, orange));

			bot = Choice("Bot plays the other robot (split screen, alone)", "bot", true, "Starts the map in split screen; the other robot is played from the record.");
			real = Choice("A real partner (I host the map myself)", "people", true, "Starts the game with the run loaded and armed. Host the map with your partner as usual and it begins there.");
			AddOption(opt, partnerCaption = new Caption(look, "PARTNER"), Down(bot, real));

			ghost = Choice("Ghost", "ghost", false, "The recorded players, shown in the level.");
			ghostColor = new ColorPick(look, "Colour") { DefaultText = "robot colours", Margin = Padding.Empty };
			ghostColor.Changed += (s, e) => { if (!building) EnableOptions(); };
			tips.SetToolTip(ghostColor, "The colour the ghosts are drawn in. Click to pick one; right-click for the robots' own colours (blue, orange; green for Chell).");
			ghostOpacity = new Stepper(look, 0, 100, 10, 0) { Margin = Padding.Empty };
			ghostOpacity.Tip(tips, "How solid the ghosts are drawn: 100 is opaque, less is see-through.");
			ghostOpacity.ValueChanged += (s, e) => { if (!building) EnableOptions(); };
			clock = Choice("On-screen clock", "clock", false, "The run's clock and how far ahead or behind you are.");
			keys = Choice("Record's keys", "keys", false, "The keys the record holds at each moment.");
			line = Choice("Route line", "line", false, "The record's route and portal shots, drawn in the level.");
			var show = Across(ghost, clock);
			show.RowCount = 2;
			show.Controls.Add(keys, 0, 1);
			show.Controls.Add(line, 1, 1);
			AddOption(opt, new Caption(look, "SHOW"), show);
			AddOption(opt, new Caption(look, "GHOSTS"), Across(ghostColor, new Caption(look, "OPACITY %") { Margin = new Padding(P(10), 0, P(8), 0) }, ghostOpacity));

			leadIn = new Stepper(look, Options.MinLeadIn, Options.MaxLeadIn, 0.5, 1) { Margin = Padding.Empty };
			leadIn.Tip(tips, "Lead-in: seconds of the record that play before you take over.");
			speed = new Stepper(look, Options.MinSpeed, Options.MaxSpeed, 0.05, 2) { Margin = Padding.Empty };
			speed.Tip(tips, "Game speed while you practice: 1 is normal, 0.5 is half speed.");
			AddOption(opt, new Caption(look, "LEAD-IN"), Across(leadIn, Aside("s"), new Caption(look, "SPEED") { Margin = new Padding(P(22), 0, P(10), 0) }, speed, Aside("×")));
			loop = Choice("Start again automatically after the finish", "loop", false, null);
			AddOption(opt, new Caption(look, "AFTER"), Across(loop));
			foreach (Control c in opt.Controls) if (c is Caption) c.Height = P(26);

			extra = new TextField(look, null, "Added to the game's command line") { Anchor = AnchorStyles.Left | AnchorStyles.Right, Margin = new Padding(0, P(2), 0, 0), Height = P(26) };
			tips.SetToolTip(extra.Box, "Your own launch options, added after the launcher's, for example  -vulkan +exec mycfg");
			var extraCaption = new Caption(look, "LAUNCH OPTIONS");
			extraCaption.Margin = new Padding(0, P(2), 0, 0);
			AddOption(opt, extraCaption, extra);

			var grid = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, Margin = Padding.Empty, BackColor = Theme.Panel };
			grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			foreach (var c in new Control[] { sign, Rule(P(7), P(4)), partsCaption, parts, notes, Rule(P(7), P(6)), opt }) {
				grid.RowStyles.Add(c == parts ? new RowStyle(SizeType.Percent, 100) : new RowStyle(SizeType.AutoSize));
				grid.Controls.Add(c, 0, grid.RowStyles.Count - 1);
			}

			launch = new SignButton(look, "LAUNCH") { Primary = true, Dock = DockStyle.Fill };
			launch.Click += (s, e) => Launch();
			var launchRow = new Panel { Dock = DockStyle.Bottom, Height = P(60), Padding = new Padding(0, P(12), 0, 0), BackColor = Theme.Panel };
			launchRow.Controls.Add(launch);

			// when the window is too low for everything (a laptop screen at a high scale) the part above Launch
			// keeps its least height and scrolls
			grid.Dock = DockStyle.None;
			var scroller = new ScrollBox(look, grid) { Dock = DockStyle.Fill };
			// its least height: everything but the parts list, and two of that list's rows. (Worked out whenever the
			// layout has just given the parts list a new height: what is left of the grid is the rest.)
			parts.SizeChanged += (s, e) => scroller.Least = grid.Height - parts.Height + P(70);
			scroller.FollowFocus();

			var card = new Card { Dock = DockStyle.Right, Width = P(SideWidth), Padding = new Padding(P(20), P(14), P(20), P(18)) };
			card.Controls.Add(scroller);
			card.Controls.Add(launchRow);
			return card;
		}

		Control BuildStatusBar() {
			status = new Words(look, "", look.Body, Theme.Ink) { Dock = DockStyle.Fill, Margin = Padding.Empty, BackColor = Theme.Field, Lamp = true };
			progress = new TickBar(look) { Anchor = AnchorStyles.None, Margin = new Padding(P(12), 0, 0, 0), Visible = false, BackColor = Theme.Field };
			progressShown = false;
			libraryLabel = new Words(look, "", look.Body, Theme.Grey) { Anchor = AnchorStyles.Right, Margin = new Padding(P(14), 0, 0, 0), BackColor = Theme.Field, Align = ContentAlignment.MiddleRight };
			var bar = new TableLayoutPanel { Dock = DockStyle.Bottom, Height = P(30), ColumnCount = 3, RowCount = 1, Padding = new Padding(P(14), 0, P(14), P(2)), BackColor = Theme.Field };
			bar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			bar.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			bar.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			bar.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
			bar.Controls.Add(status, 0, 0);
			bar.Controls.Add(progress, 1, 0);
			bar.Controls.Add(libraryLabel, 2, 0);
			return bar;
		}

		void FillDemosMenu() {
			var items = demosMenu.DropDownItems;
			items.Clear();
			items.Add(new ToolStripMenuItem("&Add folder…", null, (s, e) => AddFolder()) { ShortcutKeyDisplayString = "Ctrl+O" });
			items.Add(new ToolStripMenuItem("&Read the folders again", null, (s, e) => Rescan()) { ShortcutKeyDisplayString = "F5" });
			items.Add(new ToolStripSeparator());
			if (settings.Folders.Count == 0) items.Add(new ToolStripMenuItem("(no folders yet)") { Enabled = false });
			foreach (var f in settings.Folders) {
				string folder = f;
				bool there = Directory.Exists(folder);
				var item = new ToolStripMenuItem(folder.Replace("&", "&&") + (there ? "" : "   (not there)"));
				item.DropDownItems.Add(new ToolStripMenuItem("Remove from the list", null, (s, e) => RemoveFolder(folder)));
				item.DropDownItems.Add(new ToolStripMenuItem("Show in Explorer", null, (s, e) => ShowInExplorer(folder)) { Enabled = there });
				Dress(item);
				items.Add(item);
			}
			items.Add(new ToolStripSeparator());
			items.Add(new ToolStripMenuItem("E&xit", null, (s, e) => Close()));
			Dress(demosMenu);
		}

		void FillGameMenu() {
			var items = gameMenu.DropDownItems;
			items.Clear();
			bool found = GameLocator.IsGameDir(settings.GameDir);
			items.Add(new ToolStripMenuItem(found ? settings.GameDir.Replace("&", "&&") : "(Portal 2 not found)") { Enabled = false });
			items.Add(new ToolStripMenuItem("&Choose Portal 2 folder…", null, (s, e) => ChooseGame()));
			items.Add(new ToolStripMenuItem("&Find it through Steam", null, (s, e) => FindGame()));
			items.Add(new ToolStripSeparator());
			items.Add(new ToolStripMenuItem("&Remove plugin from game", null, (s, e) => RemovePlugin()) { Enabled = found });
			Dress(gameMenu);
		}

		// a drop-down in the window's face, without the empty strip menus keep for pictures
		void Dress(ToolStripMenuItem owner) {
			var drop = owner.DropDown as ToolStripDropDownMenu;
			if (drop == null) return;
			drop.Font = look.Body;
			drop.ShowImageMargin = false;
			drop.BackColor = Theme.Panel;
		}

		// ---- small builders ----

		// a pictogram choice: one of a group (radio) or on its own (a switch)
		Option Choice(string text, string icon, bool radio, string tip) {
			var o = new Option(look, text, icon, radio) { Margin = new Padding(0, 0, P(8), 0) };
			o.CheckedChanged += (s, e) => { if (!building) EnableOptions(); };
			if (tip != null) tips.SetToolTip(o, tip);
			return o;
		}

		// a few grey words after a control
		Words Aside(string text) {
			return new Words(look, text, look.Body, Theme.Grey) { Anchor = AnchorStyles.Left, Margin = new Padding(P(8), 0, 0, 0) };
		}

		// a thin black rule across the sign
		Control Rule(int above, int below) {
			return new Panel { Height = Math.Max(1, P(1)), BackColor = Theme.Ink, Anchor = AnchorStyles.Left | AnchorStyles.Right, Margin = new Padding(0, above, 0, below) };
		}

		// controls side by side (each in its own column, so they line up by their middles); also what makes a
		// set of radio choices one group
		TableLayoutPanel Across(params Control[] controls) {
			var p = new TableLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Anchor = AnchorStyles.Left, Margin = new Padding(0, P(2), 0, P(2)),
				ColumnCount = controls.Length, RowCount = 1, BackColor = Theme.Panel };
			for (int i = 0; i < controls.Length; i++) {
				p.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
				controls[i].Anchor = AnchorStyles.Left;
				p.Controls.Add(controls[i], i, 0);
			}
			return p;
		}

		TableLayoutPanel Down(params Control[] controls) {
			var p = new TableLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Anchor = AnchorStyles.Left, Margin = new Padding(0, P(2), 0, P(2)),
				ColumnCount = 1, RowCount = controls.Length, BackColor = Theme.Panel };
			for (int i = 0; i < controls.Length; i++) p.Controls.Add(controls[i], 0, i);
			return p;
		}

		// a row of the options: the caption level with the first line of what it names
		void AddOption(TableLayoutPanel grid, Control caption, Control control) {
			grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));
			int row = grid.RowStyles.Count - 1;
			caption.Anchor = AnchorStyles.Left | AnchorStyles.Top;
			if (caption.Margin == Padding.Empty || caption.Margin == new Padding(3)) caption.Margin = new Padding(0, control.Margin.Top, 0, 0);
			grid.Controls.Add(caption, 0, row);
			grid.Controls.Add(control, 1, row);
		}

		// ---- size, place and scale ----

		void PlaceWindow() {
			var size = new Size(P(settings.Width > 0 ? settings.Width : DefaultWidth), P(settings.Height > 0 ? settings.Height : DefaultHeight));
			bool remembered = settings.Left != int.MinValue && !App.Headless;
			// where it was left; the first time, on the monitor the mouse is on
			var screen = remembered ? Screen.FromRectangle(new Rectangle(settings.Left, settings.Top, size.Width, size.Height)) : Screen.FromPoint(Cursor.Position);
			if (App.Headless) { // a screenshot: the size asked for, as on the main monitor
				size = new Size(P(App.Size.IsEmpty ? DefaultWidth : App.Size.Width), P(App.Size.IsEmpty ? DefaultHeight : App.Size.Height));
				screen = Screen.PrimaryScreen;
			}
			var area = screen.WorkingArea;
			MinimumSize = new Size(Math.Min(P(MinWidth), area.Width), Math.Min(P(MinHeight), area.Height));
			size = new Size(Math.Min(size.Width, area.Width), Math.Min(size.Height, area.Height));
			var at = remembered ? new Point(settings.Left, settings.Top) : new Point(area.X + (area.Width - size.Width) / 2, area.Y + (area.Height - size.Height) / 2);
			// all of it on the screen, wherever it was left
			at.X = Math.Max(area.Left, Math.Min(at.X, area.Right - size.Width));
			at.Y = Math.Max(area.Top, Math.Min(at.Y, area.Bottom - size.Height));
			Bounds = new Rectangle(at, size);
			if (settings.Maximized && !App.Headless) WindowState = FormWindowState.Maximized;
		}

		protected override void OnLoad(EventArgs e) {
			base.OnLoad(e);
			// built for the main monitor's scale; this monitor's may be another
			float here = Native.ScaleOf(this);
			if (Math.Abs(here - S) > 0.01f) {
				var b = WindowState == FormWindowState.Normal ? Bounds : RestoreBounds;
				Rescale(here, new Rectangle(b.X, b.Y, (int)(b.Width * here / S), (int)(b.Height * here / S)));
			}
		}

		protected override void WndProc(ref Message m) {
			const int WM_DPICHANGED = 0x02E0;
			if (m.Msg == WM_DPICHANGED && !App.Headless) {
				// moved to a monitor with another scale (or the scale was changed): Windows says where the window should go
				var r = (Native.RECT)Marshal.PtrToStructure(m.LParam, typeof(Native.RECT));
				Rescale(((int)(long)m.WParam & 0xFFFF) / 96f, Rectangle.FromLTRB(r.Left, r.Top, r.Right, r.Bottom));
				m.Result = IntPtr.Zero;
				return;
			}
			base.WndProc(ref m);
		}

		// Builds the window again for another scale, keeping what it shows.
		void Rescale(float scale, Rectangle bounds) {
			if (Math.Abs(scale - S) < 0.01f) { if (WindowState == FormWindowState.Normal) Bounds = bounds; return; }
			ReadOptions();
			var oldControls = Controls.Cast<Control>().ToList();
			var oldLook = look;
			var oldTips = tips;
			S = scale;
			SuspendLayout();
			Controls.Clear();
			BuildUi();
			var area = Screen.FromRectangle(bounds).WorkingArea;
			MinimumSize = new Size(Math.Min(P(MinWidth), area.Width), Math.Min(P(MinHeight), area.Height));
			if (WindowState == FormWindowState.Normal) Bounds = bounds;
			ResumeLayout(true);
			foreach (var c in oldControls) c.Dispose();
			oldTips.Dispose();
			oldLook.Dispose();
			if (selected != null) list.SelectRow(rows.FindIndex(r => r.Key == selected.Key), true); // (in view again, now that the list has its size)
		}

		protected override void OnFormClosing(FormClosingEventArgs e) {
			base.OnFormClosing(e);
			if (e.Cancel) return;
			poll.Stop();
			ReadOptions();
			var b = WindowState == FormWindowState.Normal ? Bounds : RestoreBounds;
			settings.Maximized = WindowState == FormWindowState.Maximized;
			settings.Width = (int)Math.Round(b.Width / S);
			settings.Height = (int)Math.Round(b.Height / S);
			settings.Left = b.Left;
			settings.Top = b.Top;
			settings.Save();
		}

		protected override bool ProcessCmdKey(ref Message msg, Keys keyData) {
			switch (keyData) {
				case Keys.F5: Rescan(); return true;
				case Keys.Control | Keys.O: AddFolder(); return true;
				case Keys.Control | Keys.F: search.TakeFocus(); return true;
			}
			return base.ProcessCmdKey(ref msg, keyData);
		}

		// ---- the list ----

		void Poll() {
			int version = library.Version;
			// (a very big library is put in order less often while it is still being read)
			double every = allRuns.Count > 20000 ? 2000 : 250;
			if (version != shownVersion && (!library.Busy || (DateTime.UtcNow - lastRefresh).TotalMilliseconds >= every)) {
				shownVersion = version;
				lastRefresh = DateTime.UtcNow;
				allRuns = Run.Build(library.Snapshot());
				ApplyRows();
				if (!library.Busy) {
					// demos that weren't read because the helper itself failed: say so once, it is not the demos' fault
					var failed = allRuns.Where(r => !r.Ok && r.Demo.Scan.Transient).ToList();
					if (failed.Count > 0 && RouteCli.Exists) Say(failed.Count + (failed.Count == 1 ? " demo wasn't" : " demos weren't") + " read: " + failed[0].Demo.Scan.Error + ". Press F5 to try again.", true);
				}
			}
			string problem = library.TakeProblem();
			if (problem != null) Say(problem, true);
			ShowProgress();
		}

		// Filters and sorts the runs into the list, keeping the selection on the same run.
		void ApplyRows(bool reveal = false) {
			var terms = searchText.ToLowerInvariant().Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
			var shown = terms.Length == 0 ? new List<Run>(allRuns) : allRuns.Where(r => terms.All(t => r.SearchText.Contains(t))).ToList();
			shown.Sort(Run.Order(settings.SortColumn, settings.SortDescending != 0));

			int index = -1;
			if (selected != null) {
				string key = selected.Key, path = selected.Demo.Path;
				index = shown.FindIndex(r => r.Key == key);
				// (a co-op demo listed alone becomes a pair when its partner's demo has been read)
				if (index < 0) index = shown.FindIndex(r => r.Demo.Path == path || (r.Partner != null && r.Partner.Path == path));
			}
			rows = shown;
			list.Count = rows.Count;
			list.SortColumn = settings.SortColumn;
			list.SortDescending = settings.SortDescending != 0;
			list.SelectRow(index, reveal);

			var now = index >= 0 ? rows[index] : null;
			if (now == null || selected == null || now.Key != selected.Key) ShowRun(now);
			else selected = now;

			if (rows.Count > 0) list.Hint = "";
			else if (allRuns.Count > 0) list.Hint = "No demo matches \"" + searchText.Trim() + "\".";
			else if (library.Busy) list.Hint = "Reading the demo folders…";
			else if (settings.Folders.Count == 0) list.Hint = "No demo folders yet.\nAdd one with Add folder, or drop a folder of demos here.";
			else {
				int gone = settings.Folders.Count(f => !Directory.Exists(f));
				list.Hint = (gone == 0 ? "No demos in the folders yet." : gone == settings.Folders.Count ? "The demo folders in the list aren't there (see the Demos menu)."
					: "No demos in the folders yet; " + gone + " of them " + (gone == 1 ? "isn't" : "aren't") + " there (see the Demos menu).")
					+ "\nAdd a folder of demos with Add folder, or drop one here.";
			}

			int demos = 0, unread = 0;
			foreach (var r in allRuns) { demos += r.Partner != null ? 2 : 1; if (!r.Ok) unread++; }
			libraryLabel.Text = allRuns.Count == 0 ? "" : (allRuns.Count - unread) + (allRuns.Count - unread == 1 ? " run" : " runs") + " from " + demos + (demos == 1 ? " demo" : " demos")
				+ (unread > 0 ? ", " + unread + " can't be read" : "");
			libraryLabel.Fit();
		}

		void SyncSelection() {
			if (building) return;
			int i = list.Selected;
			var r = i >= 0 && i < rows.Count ? rows[i] : null;
			if (r == null ? selected != null : selected == null || r.Key != selected.Key) ShowRun(r);
		}

		void ShowProgress() {
			int done, total;
			library.Progress(out done, out total);
			bool show = library.Busy && total > 0;
			if (show) {
				progress.Set(done, total);
				if (message.Length == 0) status.Text = "Reading demos: " + done + " of " + total;
			}
			if (show != progressShown) {
				progressShown = show;
				progress.Visible = show;
				if (!show) ShowStatus();
			}
		}

		// ---- the selected run ----

		void ShowRun(Run r) {
			selected = r;
			parts.SetParts(null, -1);
			parts.Hint = "";
			notes.Visible = false;
			sign.Parts = new List<RoutePart>();
			sign.Start = -1;
			sign.Players.Clear();
			sign.Problem = null;
			sign.Empty = r == null || !r.Ok;
			sign.Time = "";
			if (r == null) {
				sign.Title = "NO RUN SELECTED";
				sign.Sub = rows.Count > 0 ? "CHOOSE A RUN IN THE LIST" : "";
			} else if (!r.Ok) {
				sign.Title = r.Demo.Name + ".dem";
				sign.Sub = "THIS DEMO CAN'T BE READ";
				sign.Problem = r.Demo.Scan.Error ?? "";
			} else {
				var s = r.Demo.Scan;
				sign.Time = r.TimeText;
				sign.Title = r.MapName.ToUpper();
				sign.Sub = s.Map.ToUpper() + "   ·   " + (s.Coop ? "CO-OP" : "SINGLE PLAYER") + (s.HasTime ? "" : s.Time > 0 ? "   ·   DEMO LENGTH" : "   ·   NO TIME");
				if (!s.Coop) sign.Players.Add(new RunHeader.Player { Name = s.Player });
				else if (r.Partner != null) {
					sign.Players.Add(new RunHeader.Player { Chip = Theme.Blue, Name = s.Player });
					sign.Players.Add(new RunHeader.Player { Chip = Theme.Orange, Name = r.Partner.Scan.Player });
				} else {
					sign.Players.Add(new RunHeader.Player { Chip = s.Host ? Theme.Blue : Theme.Orange, Name = s.Player });
					sign.Players.Add(new RunHeader.Player { Name = "the partner's demo isn't in the folders", Aside = true });
				}
				WantInfo(r);
			}
			sign.Invalidate();
			tips.SetToolTip(sign, r != null ? r.Demo.Path + (r.Partner != null ? "\n" + r.Partner.Path : "") : null);
			EnableOptions();
		}

		// Shows the run's parts, asking the helper for them (in the background, one run at a time) if they
		// haven't been read yet.
		void WantInfo(Run r) {
			RouteInfo info;
			if (infos.TryGetValue(r.Key, out info)) { ShowInfo(r, info); return; }
			parts.Hint = "Reading the run…";
			if (infoBusy) return; // when that one is back, the run selected by then is asked for
			infoBusy = true;
			string key = r.Key, demo = r.Demo.Path, partner = r.Partner != null ? r.Partner.Path : null;
			ThreadPool.QueueUserWorkItem(_ => {
				RouteInfo result;
				try { result = RouteCli.Info(demo, partner); }
				catch (Exception ex) { result = new RouteInfo { Error = ex.Message }; } // (nothing a demo does may take the launcher down)
				try {
					BeginInvoke((Action)(() => {
						infoBusy = false;
						if (infos.Count > 500) infos.Clear();
						infos[key] = result;
						if (selected != null && selected.Ok) WantInfo(selected);
					}));
				} catch (InvalidOperationException) { } // the window was closed meanwhile
			});
		}

		void ShowInfo(Run r, RouteInfo info) {
			if (!info.Ok) { parts.SetParts(null, -1); parts.Hint = "The run's parts couldn't be read (" + info.Error + ").\nIt can still be started from the beginning."; }
			else if (info.Parts.Count == 0) { parts.SetParts(null, -1); parts.Hint = "This run isn't split into parts: it starts from the beginning."; }
			else {
				parts.Hint = "";
				int row = partRun == r.Key ? info.Parts.FindIndex(p => p.Index == partChosen) : 0;
				parts.SetParts(info.Parts, Math.Max(0, row));
			}
			sign.Parts = info.Ok ? info.Parts : new List<RoutePart>();
			sign.Start = parts.Selected;

			if (info.Ok && info.Players.Count > 0) {
				sign.Players.Clear();
				foreach (var p in info.Players)
					sign.Players.Add(new RunHeader.Player { Chip = p.Team == "blue" ? Theme.Blue : p.Team == "orange" ? Theme.Orange : Color.Empty,
						Name = p.Name + (p.HasInputs ? "" : " (not recorded)") });
			}
			sign.Invalidate();
			notes.Text = info.Ok ? string.Join("  ", info.Notes) : "";
			notes.Visible = notes.Text.Length > 0;
			tips.SetToolTip(notes, info.Ok && info.Notes.Count > 0 ? string.Join("\n", info.Notes) : null);
		}

		int SelectedPart { get { return parts.Part(parts.Selected); } }

		// ---- options ----

		void ShowOptions() {
			var o = settings.Options;
			building = true;
			(o.Watch ? watch : practice).Checked = true;
			(o.Orange ? orange : blue).Checked = true;
			(o.RealPartner ? real : bot).Checked = true;
			ghost.Checked = o.Ghost;
			ghostColor.Chosen = ColorOf(o.GhostColor);
			ghostOpacity.Value = o.GhostOpacity;
			clock.Checked = o.Clock;
			keys.Checked = o.Keys;
			line.Checked = o.Line;
			loop.Checked = o.Loop;
			leadIn.Value = o.LeadIn;
			speed.Value = o.Speed;
			extra.Box.Text = o.Extra;
			building = false;
			EnableOptions();
		}

		void ReadOptions() {
			var o = settings.Options;
			o.Watch = watch.Checked;
			o.Orange = orange.Checked;
			o.RealPartner = real.Checked;
			o.Ghost = ghost.Checked;
			o.GhostColor = ghostColor.Chosen.HasValue ? ghostColor.Chosen.Value.R + " " + ghostColor.Chosen.Value.G + " " + ghostColor.Chosen.Value.B : "";
			ghostOpacity.Bump(0);
			o.GhostOpacity = ghostOpacity.Value;
			o.Clock = clock.Checked;
			o.Keys = keys.Checked;
			o.Line = line.Checked;
			o.Loop = loop.Checked;
			leadIn.Bump(0); // (takes a number still being typed)
			speed.Bump(0);
			o.LeadIn = leadIn.Value;
			o.Speed = speed.Value;
			o.Extra = extra.Box.Text.Trim();
		}

		// The co-op choices only mean something for a co-op run, and the robot only when a bot plays the other
		// one. The portal colour everything is marked with follows: orange when the player is about to be
		// P-body, blue otherwise (ATLAS, or single player).
		// "r g b" as a colour, or null for the default
		static Color? ColorOf(string text) {
			string parsed = Options.ParseColor(text);
			if (parsed == "") return null;
			var p = parsed.Split(' ');
			return Color.FromArgb(int.Parse(p[0]), int.Parse(p[1]), int.Parse(p[2]));
		}

		void EnableOptions() {
			if (building || launch == null) return;
			bool ok = selected != null && selected.Ok, coop = ok && selected.Coop;
			partnerCaption.Enabled = bot.Enabled = real.Enabled = coop;
			roleCaption.Enabled = blue.Enabled = orange.Enabled = coop && bot.Checked;
			ghostColor.Enabled = ghostOpacity.Enabled = ghost.Checked;
			launch.Enabled = ok;
			var accent = coop && bot.Checked && orange.Checked ? Theme.Orange : Theme.Blue;
			if (look.Accent != accent) {
				look.Accent = accent;
				foreach (var c in new Control[] { list, parts, sign, launch }) c.Invalidate();
			}
		}

		// ---- launching ----

		public void Launch() {
			if (selected == null || !selected.Ok) return;
			ReadOptions();
			settings.Save();
			var request = new LaunchRequest {
				GameDir = settings.GameDir, Demo = selected.Demo.Path, Partner = selected.Partner != null ? selected.Partner.Path : null,
				Map = selected.Map, MapName = selected.MapName, Coop = selected.Coop, Part = SelectedPart, Options = settings.Options,
			};
			Cursor = Cursors.WaitCursor;
			LaunchResult result;
			try { result = GameLauncher.Launch(request); } finally { Cursor = Cursors.Default; }
			Say(result.Message, !result.Ok);
		}

		public void RemovePlugin() {
			Say(GameLauncher.RemovePlugin(settings.GameDir));
		}

		// ---- folders and the game ----

		void AddFolder() {
			using (var dlg = new FolderBrowserDialog { Description = "A folder of demos (its subfolders are searched too)", ShowNewFolderButton = false }) {
				if (dlg.ShowDialog(this) == DialogResult.OK) AddFolders(new[] { dlg.SelectedPath });
			}
		}

		// a dropped folder is added; a dropped demo adds the folder it is in
		static IEnumerable<string> DroppedFolders(string[] paths) {
			foreach (var p in paths ?? new string[0]) {
				if (Directory.Exists(p)) yield return p;
				else if (p.EndsWith(".dem", StringComparison.OrdinalIgnoreCase) && File.Exists(p)) yield return Path.GetDirectoryName(p);
			}
		}

		void AddFolders(IEnumerable<string> folders) {
			int added = 0, known = 0;
			string last = null;
			foreach (var f in folders) {
				if (string.IsNullOrEmpty(f)) continue;
				if (settings.HasFolder(f)) { known++; continue; }
				settings.Folders.Add(last = f);
				added++;
			}
			if (added == 0) { Say(known > 0 ? "That folder is already in the list." : "Nothing to add: drop a folder of demos, or a demo."); return; }
			settings.FoldersSaved = true;
			settings.Save();
			Say(added == 1 ? "Added " + last : "Added " + added + " folders");
			library.Start(settings.Folders);
		}

		void RemoveFolder(string folder) {
			settings.Folders.RemoveAll(f => Settings.SamePath(f, folder));
			settings.FoldersSaved = true;
			settings.Save();
			Say("Removed " + folder + " from the list (its demos stay where they are)");
			library.Start(settings.Folders);
		}

		void Rescan() {
			infos.Clear();
			Say("");
			library.Start(settings.Folders);
		}

		static void ShowInExplorer(string folder) {
			try { using (Process.Start("explorer.exe", "\"" + folder + "\"")) { } }
			catch (System.ComponentModel.Win32Exception) { }
		}

		void ChooseGame() {
			using (var dlg = new FolderBrowserDialog { Description = "The Portal 2 folder (the one with portal2.exe)", ShowNewFolderButton = false, SelectedPath = settings.GameDir ?? "" }) {
				if (dlg.ShowDialog(this) != DialogResult.OK) return;
				string dir = dlg.SelectedPath;
				// (the "portal2" folder inside it is an easy one to pick by mistake)
				if (!GameLocator.IsGameDir(dir) && GameLocator.IsGameDir(Path.GetDirectoryName(dir))) dir = Path.GetDirectoryName(dir);
				if (!GameLocator.IsGameDir(dir)) { Say("There is no portal2.exe in " + dir + ", so that isn't the Portal 2 folder.", true); return; }
				UseGame(dir, true);
			}
		}

		void FindGame() {
			string dir = GameLocator.Find();
			if (dir == null) Say("Steam doesn't list a Portal 2 install. Choose the folder by hand.", true);
			else UseGame(dir, false);
		}

		void UseGame(string dir, bool chosen) {
			settings.GameDir = dir;
			settings.GameChosen = chosen;
			// its demos folder joins the list, like on a first start
			string demos = Path.Combine(dir, "portal2", "demos");
			bool added = Directory.Exists(demos) && !settings.HasFolder(demos);
			if (added) settings.Folders.Add(demos);
			settings.Save();
			Say("Portal 2 folder: " + dir);
			if (added) library.Start(settings.Folders);
		}

		// ---- the status line ----

		void Say(string text, bool error = false) {
			message = text ?? "";
			messageIsError = error;
			if (App.DryRun && message.Length > 0) App.Log("status line" + (error ? " (error)" : "") + ": " + message);
			ShowStatus();
		}

		void ShowStatus() {
			if (status == null) return;
			status.ForeColor = messageIsError && message.Length > 0 ? Theme.Alert : Theme.Ink;
			status.Text = message.Length > 0 ? message : "Choose a run, how to practice it, then Launch.";
			tips.SetToolTip(status, message.Length > 0 ? message : null); // the whole text, should it not fit
		}

		// ---- for the screenshot run (Program.ScreenshotRun): the window is never shown ----

		// Creates the window and every control in it without showing anything: what showing a window does
		// first (Control.CreateControl), told not to mind that nothing is visible.
		public void CreateHidden() {
			var create = typeof(Control).GetMethod("CreateControl", BindingFlags.Instance | BindingFlags.NonPublic, null, new[] { typeof(bool) }, null);
			if (create == null) throw new NotSupportedException("this .NET Framework can't create a window without showing it");
			create.Invoke(this, new object[] { true });
			PerformLayout();
		}

		public bool LibraryIdle { get { return !library.Busy && shownVersion == library.Version; } }
		public bool RunReady { get { return selected == null || !selected.Ok || infos.ContainsKey(selected.Key); } }

		public void SelectRow(int index) {
			if (index < 0 || index >= rows.Count) return;
			list.Pick(index);
		}

		// as if the window had been dragged to a monitor with this scale
		public void RescaleTo(int percent) {
			float f = percent / 100f / S;
			Rescale(percent / 100f, new Rectangle(Left, Top, (int)(Width * f), (int)(Height * f)));
			CreateHidden(); // (a window that isn't showing doesn't create its new controls by itself)
		}

		public void SelectPart(int number) {
			int row = parts.RowOf(number);
			if (row >= 0) parts.Pick(row);
		}

		// Does what the player's mouse and keys would do, by calling the controls' own handlers (the real mouse
		// and keyboard are never touched), and writes what happened to the log.
		public void SelfTest() {
			Action<string> say = text => App.Log("self-test: " + text);
			Func<string> where = () => "row " + list.Selected + (selected != null ? " (" + selected.MapName + ", " + selected.TimeText + ")" : "");
			Func<string> accent = () => look.Accent == Theme.Orange ? "orange" : "blue";

			// the selected run's parts and the options first (its parts are read by now)
			say(where() + ": " + parts.Count + " parts, start part " + SelectedPart + ", accent " + accent());
			parts.Key(Keys.Down); say("Down in the parts -> start part " + SelectedPart + ", sign shows " + (sign.Start + 1));
			parts.ClickAt(new Point(P(60), P(31) * 3 + P(5))); say("click on the fourth part -> start part " + SelectedPart);
			parts.Key(Keys.Home); say("Home -> start part " + SelectedPart);
			watch.Press(); say("Watch pressed -> watch " + watch.Checked + ", practice " + practice.Checked);
			practice.Press(); say("Practice pressed -> watch " + watch.Checked + ", practice " + practice.Checked);
			orange.Press(); say("Orange pressed -> orange " + orange.Checked + ", blue " + blue.Checked + ", accent " + accent());
			real.Press(); say("Real partner pressed -> role can be chosen: " + blue.Enabled + ", accent " + accent());
			bot.Press(); say("Bot pressed -> role can be chosen: " + blue.Enabled + ", accent " + accent());
			blue.Press(); say("Blue pressed -> accent " + accent());
			ghost.Press(); say("Ghost pressed -> " + ghost.Checked);
			ghost.Press(); say("again -> " + ghost.Checked);
			leadIn.Bump(1); say("lead-in + -> " + leadIn.Value);
			leadIn.Bump(-1); leadIn.Bump(-1); say("lead-in -, - -> " + leadIn.Value);
			speed.Bump(-1); say("speed - -> " + speed.Value);
			speed.Value = 99; say("speed set to 99 -> " + speed.Value);
			speed.Value = 1; leadIn.Value = 1.5;

			// then the list
			say("list has " + list.Count + " rows");
			list.Key(Keys.Home); say("Home -> " + where());
			list.Key(Keys.Down); list.Key(Keys.Down); say("Down, Down -> " + where());
			list.Key(Keys.End); say("End -> " + where());
			list.Key(Keys.Up); say("Up -> " + where());
			list.ScrollBy(-100000);
			list.ClickAt(new Point(P(60), P(30) + P(25) * 3 + P(4))); say("click on the fourth row -> " + where() + "; co-op choices enabled: " + bot.Enabled);
			list.ClickAt(new Point(list.Width - P(40), P(10))); say("click on the File column head -> sorted by column " + settings.SortColumn + (settings.SortDescending != 0 ? " descending" : "") + "; " + where());
			list.ClickAt(new Point(list.Width - P(40), P(10))); say("again -> column " + settings.SortColumn + (settings.SortDescending != 0 ? " descending" : "") + "; " + where());
			list.ClickAt(new Point(P(20), P(10))); say("click on the Map column head -> column " + settings.SortColumn + (settings.SortDescending != 0 ? " descending" : "") + "; " + where());

			// and, for the picture: the progress strip as it is while demos are being read
			progress.Set(3, 8);
			progress.Visible = true;
		}

		// A picture of a menu's drop-down, laid out and drawn without being opened.
		public void SaveMenuPicture(string file) {
			var drop = demosMenu.DropDown;
			var create = typeof(Control).GetMethod("CreateControl", BindingFlags.Instance | BindingFlags.NonPublic, null, new[] { typeof(bool) }, null);
			create.Invoke(drop, new object[] { true });
			drop.Size = drop.GetPreferredSize(Size.Empty);
			drop.PerformLayout();
			using (var picture = new Bitmap(drop.Width, drop.Height, PixelFormat.Format24bppRgb)) {
				drop.DrawToBitmap(picture, new Rectangle(0, 0, drop.Width, drop.Height));
				picture.Save(file, ImageFormat.Png);
			}
		}

		public void SavePicture(string file) {
			CreateHidden(); // (controls added since)
			Application.DoEvents();
			using (var whole = new Bitmap(Width, Height, PixelFormat.Format24bppRgb)) { // (no alpha: what GDI draws has none)
				DrawToBitmap(whole, new Rectangle(0, 0, Width, Height));
				// the client area only: the frame of a window that was never shown is drawn in the old plain style
				var origin = PointToScreen(Point.Empty);
				var client = new Rectangle(origin.X - Left, origin.Y - Top, ClientSize.Width, ClientSize.Height);
				client.Intersect(new Rectangle(0, 0, whole.Width, whole.Height));
				using (var picture = whole.Clone(client, whole.PixelFormat)) {
					Directory.CreateDirectory(Path.GetDirectoryName(file));
					picture.Save(file, ImageFormat.Png);
				}
			}
		}

		protected override void Dispose(bool disposing) {
			if (disposing) {
				poll.Dispose();
				if (tips != null) tips.Dispose();
				if (look != null) look.Dispose();
			}
			base.Dispose(disposing);
		}
	}
}
