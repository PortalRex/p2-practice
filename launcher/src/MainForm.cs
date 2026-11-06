// The window: the demos on the left; the selected run, its parts, the options and Launch on the right; a status
// line along the bottom.
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
		const int DefaultWidth = 1180, DefaultHeight = 760, MinWidth = 960, MinHeight = 690, SideWidth = 456;
		const int MapColumn = 150, TimeColumn = 62, PlayersColumn = 200, ModeColumn = 98, FileColumn = 160;
		static readonly Color Accent = Color.FromArgb(0, 103, 192);
		static readonly Color ErrorText = Color.FromArgb(176, 32, 32);

		readonly Settings settings;
		readonly DemoLibrary library = new DemoLibrary();
		readonly Timer poll = new Timer { Interval = 150 };      // picks up what the library read in the background
		readonly Timer selectDelay = new Timer { Interval = 40 }; // a click deselects, then selects: act once, on the result
		readonly Dictionary<string, RouteInfo> infos = new Dictionary<string, RouteInfo>(); // run key -> its parts, once read

		// The window is laid out by hand for one scale: every size goes through P() and every font through NewFont().
		// When the window moves to a monitor with another scale it is built again for that scale (Rescale).
		float S = 1;
		int P(float v) { return (int)Math.Round(v * S); }
		Font NewFont(string family, float points) { return new Font(family, points * S * 96f / 72f, FontStyle.Regular, GraphicsUnit.Pixel); }
		Font uiFont, titleFont, launchFont;
		ToolTip tips;

		MenuStrip menu;
		ToolStripMenuItem demosMenu, gameMenu;
		TextBox search, extra;
		ListViewEx list, parts;
		Label title, subtitle, playersLine, notes, status, libraryLabel, roleCaption, partnerCaption;
		RadioButton practise, watch, blue, orange, bot, real;
		CheckBox ghost, clock, keys, line, loop;
		NumericUpDown leadIn, speed;
		Button launch;
		ProgressBar progress;

		List<Run> allRuns = new List<Run>(), rows = new List<Run>();
		Run selected;
		string searchText = "";
		string partRun;        // the run the player chose a part of...
		int partChosen = 1;    // ...and the part
		int shownVersion = -1;
		int fittedWidth = -1;  // the list width its columns were last shared out for
		DateTime lastRefresh;
		bool building, applying, fitting, fitQueued, infoBusy, progressShown;
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
			selectDelay.Tick += (s, e) => { selectDelay.Stop(); SyncSelection(); };
			DragEnter += (s, e) => e.Effect = e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Link : DragDropEffects.None;
			DragDrop += (s, e) => AddFolders(DroppedFolders((string[])e.Data.GetData(DataFormats.FileDrop)));
			library.Start(settings.Folders);
			poll.Start();
		}

		// ---- building the window ----

		void BuildUi() {
			building = true;
			SuspendLayout();
			uiFont = NewFont("Segoe UI", 9f);
			titleFont = NewFont("Segoe UI Semibold", 15f);
			launchFont = NewFont("Segoe UI Semibold", 11.5f);
			Font = uiFont;
			BackColor = SystemColors.Control;
			tips = new ToolTip { AutoPopDelay = 15000, InitialDelay = 500, ReshowDelay = 100 };

			var listPanel = BuildLibrary();
			var rule = new Panel { Dock = DockStyle.Right, Width = Math.Max(1, P(1)), BackColor = RuleColor };
			var side = BuildSide();
			var bar = BuildStatusBar();
			menu = BuildMenu();
			// (docked from the last one added to the first: menu, status line, side, rule; the list gets the rest)
			Controls.AddRange(new Control[] { listPanel, rule, side, bar, menu });
			MainMenuStrip = menu;
			building = false;
			ResumeLayout(true);

			ShowOptions();
			ApplyRows(true);
			ShowRun(selected);
			ShowStatus();
		}

		static Color RuleColor {
			get { var a = SystemColors.Control; var b = SystemColors.ControlDark; return Color.FromArgb((a.R + b.R) / 2, (a.G + b.G) / 2, (a.B + b.B) / 2); }
		}

		Control BuildLibrary() {
			search = new TextBox { Anchor = AnchorStyles.Left | AnchorStyles.Right, Margin = new Padding(0, 0, P(8), 0), Text = searchText };
			Native.SetPrompt(search, "Search map, player or file");
			search.TextChanged += (s, e) => { if (building) return; searchText = search.Text; ApplyRows(); };
			var add = new Button { Text = "Add folder…", AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Padding = new Padding(P(8), 0, P(8), 0),
				Margin = Padding.Empty, Anchor = AnchorStyles.None, UseVisualStyleBackColor = true };
			add.Click += (s, e) => AddFolder();
			tips.SetToolTip(add, "Add a folder of demos (its subfolders are searched too). You can also drop a folder on the window.");
			var top = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 2, RowCount = 1,
				Padding = new Padding(P(10), P(10), P(10), P(9)) };
			top.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			top.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			top.Controls.Add(search, 0, 0);
			top.Controls.Add(add, 1, 0);

			list = new ListViewEx { Dock = DockStyle.Fill, BorderStyle = BorderStyle.None, VirtualMode = true, ShowItemToolTips = true };
			list.Columns.Add("Map", P(MapColumn));
			list.Columns.Add("Time", P(TimeColumn), HorizontalAlignment.Right);
			list.Columns.Add("Players", P(PlayersColumn));
			list.Columns.Add("Mode", P(ModeColumn));
			list.Columns.Add("File", P(FileColumn));
			fittedWidth = -1;
			list.RetrieveVirtualItem += (s, e) => e.Item = ItemFor(e.ItemIndex);
			list.SelectedIndexChanged += (s, e) => { if (!building && !applying) { selectDelay.Stop(); selectDelay.Start(); } };
			list.ColumnClick += (s, e) => {
				if (settings.SortColumn == e.Column) settings.SortDescending ^= 1;
				else { settings.SortColumn = e.Column; settings.SortDescending = 0; }
				ApplyRows();
			};
			list.ItemActivate += (s, e) => { SyncSelection(); Launch(); }; // double-click or Enter
			list.Resize += (s, e) => FitColumnsSoon();
			// (a list that isn't a window yet takes no selection and no sort arrow: they are given when it becomes one)
			list.Realized += (s, e) => { SelectInList(selected != null ? rows.FindIndex(r => r.Key == selected.Key) : -1, true); list.ShowSortArrow(settings.SortColumn, settings.SortDescending != 0); };

			var panel = new Panel { Dock = DockStyle.Fill };
			panel.Controls.Add(list);
			panel.Controls.Add(top);
			return panel;
		}

		Control BuildSide() {
			title = TextLine(titleFont, SystemColors.ControlText, 0);
			subtitle = TextLine(uiFont, SystemColors.GrayText, P(1));
			playersLine = TextLine(uiFont, SystemColors.ControlText, P(3));
			var partsCaption = new Label { Text = "Start from", AutoSize = true, Margin = new Padding(0, P(14), 0, P(4)) };

			parts = new ListViewEx { Dock = DockStyle.Fill, HeaderStyle = ColumnHeaderStyle.Nonclickable, Margin = Padding.Empty };
			parts.Columns.Add("#", P(32));
			parts.Columns.Add("Time", P(100));
			parts.Columns.Add("Part", P(200));
			parts.Resize += (s, e) => FitColumnsSoon();
			parts.SelectedIndexChanged += (s, e) => {
				if (building || applying || selected == null || parts.SelectedItems.Count == 0) return;
				partRun = selected.Key;
				partChosen = (int)parts.SelectedItems[0].Tag;
			};
			parts.ItemActivate += (s, e) => Launch();

			notes = new Label { AutoSize = true, Anchor = AnchorStyles.Left | AnchorStyles.Right, ForeColor = SystemColors.GrayText, UseMnemonic = false,
				Margin = new Padding(0, P(5), 0, 0), MaximumSize = new Size(0, uiFont.Height * 2 + P(2)), AutoEllipsis = true, Visible = false };

			// the options: a caption column and the controls
			var opt = new TableLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Dock = DockStyle.Fill, ColumnCount = 2,
				Margin = new Padding(0, P(12), 0, 0) };
			opt.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			opt.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));

			practise = Radio("Practise", "Play it yourself: the record runs up to the part you chose, then you take over.");
			watch = Radio("Watch the run", "Ride along with the recorded run from the part you chose.");
			AddOption(opt, Caption("Mode"), Across(practise, watch));

			blue = Radio("Blue (ATLAS)", null);
			orange = Radio("Orange (P-body)", null);
			AddOption(opt, roleCaption = Caption("Play as"), Across(blue, orange));

			bot = Radio("Bot plays the other robot (split screen, alone)", "Starts the map in split screen; the other robot is played from the record.");
			real = Radio("A real partner (I host the map myself)", "Starts the game with the run loaded and armed. Host the map with your partner as usual and it begins there.");
			var partner = Down(bot, real);
			AddOption(opt, partnerCaption = CaptionBeside("Partner", bot, partner), partner);

			ghost = Check("Ghost", "The recorded players, shown in the level.");
			clock = Check("On-screen clock", "The run's clock and how far ahead or behind you are.");
			keys = Check("Record's keys", "The keys the record holds at each moment.");
			line = Check("Route line", "The record's route and portal shots, drawn in the level.");
			var show = Across(ghost, clock);
			show.RowCount = 2;
			show.Controls.Add(keys, 0, 1);
			show.Controls.Add(line, 1, 1);
			AddOption(opt, CaptionBeside("Show", ghost, show), show);

			leadIn = Number(Options.MinLeadIn, Options.MaxLeadIn, 0.5, 1, "Seconds of the record that play before you take over.");
			AddOption(opt, Caption("Lead-in"), Across(leadIn, Dim("seconds")));
			speed = Number(Options.MinSpeed, Options.MaxSpeed, 0.05, 2, "How fast the game runs while you practise. 1 is normal speed, 0.5 is half speed.");
			AddOption(opt, Caption("Game speed"), Across(speed, Dim("(1 = normal)")));
			loop = Check("Start again automatically after the finish", null);
			AddOption(opt, Caption(""), Across(loop));

			extra = new TextBox { Anchor = AnchorStyles.Left | AnchorStyles.Right, Margin = new Padding(0, P(4), 0, 0) };
			Native.SetPrompt(extra, "Added to the game's command line");
			tips.SetToolTip(extra, "Your own launch options, added after the launcher's, for example  -vulkan +exec mycfg");
			var extraCaption = Caption("Launch options");
			extraCaption.Margin = new Padding(0, P(4), P(14), 0);
			AddOption(opt, extraCaption, extra);

			var grid = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, Margin = Padding.Empty };
			grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			foreach (var c in new Control[] { title, subtitle, playersLine, partsCaption, parts, notes, opt }) {
				grid.RowStyles.Add(c == parts ? new RowStyle(SizeType.Percent, 100) : new RowStyle(SizeType.AutoSize));
				grid.Controls.Add(c, 0, grid.RowStyles.Count - 1);
			}

			launch = new Button { Text = "Launch", Dock = DockStyle.Fill, Font = launchFont, UseVisualStyleBackColor = true };
			if (!SystemInformation.HighContrast) { // the one button that matters, in the accent colour
				launch.FlatStyle = FlatStyle.Flat;
				launch.FlatAppearance.BorderSize = 0;
				launch.FlatAppearance.MouseOverBackColor = Color.FromArgb(26, 118, 201);
				launch.FlatAppearance.MouseDownBackColor = Color.FromArgb(0, 84, 160);
				launch.ForeColor = Color.White;
				launch.EnabledChanged += (s, e) => launch.BackColor = launch.Enabled ? Accent : Color.FromArgb(196, 199, 204);
				launch.BackColor = Accent;
			}
			launch.Click += (s, e) => Launch();
			var launchRow = new Panel { Dock = DockStyle.Bottom, Height = P(58), Padding = new Padding(0, P(14), 0, 0) };
			launchRow.Controls.Add(launch);

			// when the window is too low for everything (a small screen at a high scale) the part above Launch scrolls
			var scroller = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
			scroller.Controls.Add(grid);
			grid.Layout += (s, e) => {
				int needed = grid.Height - parts.Height + P(84); // everything but the parts list, and a few of its rows
				if (scroller.AutoScrollMinSize.Height != needed) scroller.AutoScrollMinSize = new Size(0, needed);
			};

			var panel = new Panel { Dock = DockStyle.Right, Width = P(SideWidth), Padding = new Padding(P(14), P(10), P(14), P(14)) };
			panel.Controls.Add(scroller);
			panel.Controls.Add(launchRow);
			return panel;
		}

		Control BuildStatusBar() {
			status = new Label { AutoSize = false, Dock = DockStyle.Fill, AutoEllipsis = true, TextAlign = ContentAlignment.MiddleLeft, UseMnemonic = false, Margin = Padding.Empty };
			progress = new ProgressBar { Width = P(130), Height = P(12), Anchor = AnchorStyles.None, Margin = new Padding(P(12), 0, 0, 0), Visible = false };
			progressShown = false;
			libraryLabel = new Label { AutoSize = true, Anchor = AnchorStyles.Right, ForeColor = SystemColors.GrayText, UseMnemonic = false, Margin = new Padding(P(12), 0, 0, 0) };
			var bar = new TableLayoutPanel { Dock = DockStyle.Bottom, Height = P(28), ColumnCount = 3, RowCount = 1, Padding = new Padding(P(10), P(1), P(10), 0) };
			bar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			bar.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			bar.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			bar.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
			bar.Controls.Add(status, 0, 0);
			bar.Controls.Add(progress, 1, 0);
			bar.Controls.Add(libraryLabel, 2, 0);
			bar.Paint += (s, e) => { using (var pen = new Pen(RuleColor)) e.Graphics.DrawLine(pen, 0, 0, bar.Width, 0); };
			return bar;
		}

		MenuStrip BuildMenu() {
			var m = new MenuStrip { Font = uiFont, Dock = DockStyle.Top, Padding = new Padding(P(4), P(2), 0, P(2)), ShowItemToolTips = false };
			demosMenu = new ToolStripMenuItem("&Demos");
			gameMenu = new ToolStripMenuItem("&Game");
			// (filled again whenever they open: the folders and the game folder change)
			demosMenu.DropDownOpening += (s, e) => FillDemosMenu();
			gameMenu.DropDownOpening += (s, e) => FillGameMenu();
			m.Items.Add(demosMenu);
			m.Items.Add(gameMenu);
			FillDemosMenu();
			FillGameMenu();
			return m;
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
				items.Add(item);
			}
			items.Add(new ToolStripSeparator());
			items.Add(new ToolStripMenuItem("E&xit", null, (s, e) => Close()));
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
		}

		// ---- small builders ----

		// one line of text that ends in "…" when the panel is too narrow for it
		Label TextLine(Font font, Color color, int above) {
			return new Label { AutoSize = false, Anchor = AnchorStyles.Left | AnchorStyles.Right, Height = font.Height + P(1), Font = font, ForeColor = color,
				AutoEllipsis = true, UseMnemonic = false, Margin = new Padding(0, above, 0, 0) };
		}

		Label Caption(string text) {
			return new Label { Text = text, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, 0, P(14), 0) };
		}

		// the caption of a group that is two lines high: level with the first line, not the middle
		Label CaptionBeside(string text, Control firstLine, Control group) {
			var c = Caption(text);
			c.Anchor = AnchorStyles.Left | AnchorStyles.Top;
			c.Margin = new Padding(0, group.Margin.Top + firstLine.Margin.Top + Math.Max(0, (firstLine.PreferredSize.Height - c.PreferredSize.Height) / 2), P(14), 0);
			return c;
		}

		// a few grey words after a control
		Label Dim(string text) {
			return new Label { Text = text, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(P(6), 0, 0, 0), ForeColor = SystemColors.GrayText };
		}

		RadioButton Radio(string text, string tip) {
			var r = new RadioButton { Text = text, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, P(1), P(18), P(1)), UseMnemonic = false };
			r.CheckedChanged += (s, e) => OptionChanged();
			if (tip != null) tips.SetToolTip(r, tip);
			return r;
		}

		CheckBox Check(string text, string tip) {
			var c = new CheckBox { Text = text, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, P(1), P(18), P(1)), UseMnemonic = false };
			c.CheckedChanged += (s, e) => OptionChanged();
			if (tip != null) tips.SetToolTip(c, tip);
			return c;
		}

		NumericUpDown Number(double min, double max, double step, int decimals, string tip) {
			var n = new NumericUpDown { Minimum = (decimal)min, Maximum = (decimal)max, Increment = (decimal)step, DecimalPlaces = decimals, Width = P(62),
				Anchor = AnchorStyles.Left, Margin = new Padding(0, P(2), 0, P(2)), TextAlign = HorizontalAlignment.Right };
			tips.SetToolTip(n, tip);
			return n;
		}

		// controls side by side (each in its own column, so they line up by their middles); also what makes a
		// set of radio buttons one group
		TableLayoutPanel Across(params Control[] controls) {
			var p = new TableLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Anchor = AnchorStyles.Left, Margin = new Padding(0, P(2), 0, P(2)),
				ColumnCount = controls.Length, RowCount = 1 };
			for (int i = 0; i < controls.Length; i++) {
				p.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
				p.Controls.Add(controls[i], i, 0);
			}
			return p;
		}

		TableLayoutPanel Down(params Control[] controls) {
			var p = new TableLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Anchor = AnchorStyles.Left, Margin = new Padding(0, P(2), 0, P(2)),
				ColumnCount = 1, RowCount = controls.Length };
			for (int i = 0; i < controls.Length; i++) p.Controls.Add(controls[i], 0, i);
			return p;
		}

		static void AddOption(TableLayoutPanel grid, Control caption, Control control) {
			grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));
			int row = grid.RowStyles.Count - 1;
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
				if (!App.Size.IsEmpty) size = new Size(P(App.Size.Width), P(App.Size.Height));
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
			var oldFonts = new[] { uiFont, titleFont, launchFont };
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
			foreach (var f in oldFonts) f.Dispose();
			oldTips.Dispose();
			if (selected != null) SelectInList(rows.FindIndex(r => r.Key == selected.Key), true); // (in view again, now that the list has its size)
		}

		// The list's columns share its width: Map and Players give way when the window is narrow and grow a little
		// when it is wide; File takes what is left.
		void FitColumns() {
			if (list == null || parts == null || building || fitting) return;
			fitting = true; // (a column's new width can change the room inside the list, which comes back here)
			try {
				int width = list.ClientSize.Width;
				var widths = new int[list.Columns.Count];
				for (int i = 0; i < widths.Length; i++) widths[i] = list.Columns[i].Width;
				if (width != fittedWidth) { // (only when the width changes: in between, the columns are the player's to drag)
					fittedWidth = width;
					double f = Math.Max(0.5, Math.Min(1.2, (width - P(TimeColumn + ModeColumn)) / (double)P(MapColumn + PlayersColumn + FileColumn)));
					widths[0] = (int)(P(MapColumn) * f);
					widths[1] = P(TimeColumn);
					widths[2] = (int)(P(PlayersColumn) * f);
					widths[3] = P(ModeColumn);
				}
				SetWidths(list, widths, P(90));
				SetWidths(parts, new[] { parts.Columns[0].Width, parts.Columns[1].Width, 0 }, P(80));
			} finally { fitting = false; }
		}

		// After a list changed size. Not at once: the size also changes while the list is working out its scroll
		// bars (one appears, the room inside shrinks), and new column widths in the middle of that leave it with
		// a scroll bar for the old ones.
		void FitColumnsSoon() {
			if (fitQueued || !IsHandleCreated) return; // (before the window exists, the first Poll does it)
			fitQueued = true;
			BeginInvoke((Action)(() => { fitQueued = false; FitColumns(); }));
		}

		// gives the columns these widths; the last one takes what the others leave
		static void SetWidths(ListView view, int[] widths, int least) {
			int last = widths.Length - 1, others = 0;
			for (int i = 0; i < last; i++) others += widths[i];
			widths[last] = Math.Max(least, view.ClientSize.Width - others);
			bool same = true;
			for (int i = 0; i <= last; i++) same &= view.Columns[i].Width == widths[i];
			if (same) return;
			view.BeginUpdate(); // (the list works its scroll bars out once, for the final widths)
			for (int i = 0; i <= last; i++) view.Columns[i].Width = widths[i];
			view.EndUpdate();
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
				case Keys.Control | Keys.F: search.Focus(); search.SelectAll(); return true;
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
			FitColumns();
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
			applying = true;
			rows = shown;
			list.VirtualListSize = rows.Count;
			applying = false;
			SelectInList(index, reveal);
			list.Invalidate();
			list.ShowSortArrow(settings.SortColumn, settings.SortDescending != 0);

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
			FitColumns();
		}

		// puts the list's selection on a row (-1: none) without that counting as the player choosing it
		void SelectInList(int index, bool reveal) {
			if (!list.IsHandleCreated) return;
			applying = true;
			list.SelectedIndices.Clear();
			if (index >= 0 && index < rows.Count) {
				list.SelectedIndices.Add(index);
				if (reveal) list.EnsureVisible(index);
			}
			applying = false;
		}

		ListViewItem ItemFor(int index) {
			if (index < 0 || index >= rows.Count) return new ListViewItem(new[] { "", "", "", "", "" });
			var r = rows[index];
			var item = new ListViewItem(new[] { r.MapName, r.TimeText, r.Players, r.Mode, r.Files });
			if (!r.Ok) item.ForeColor = SystemColors.GrayText;
			item.ToolTipText = r.Demo.Path + (r.Partner != null ? "\n" + r.Partner.Path : "") + (r.Ok ? "" : "\n" + r.Demo.Scan.Error);
			return item;
		}

		void SyncSelection() {
			if (building || applying) return;
			int i = list.SelectedIndices.Count > 0 ? list.SelectedIndices[0] : -1;
			var r = i >= 0 && i < rows.Count ? rows[i] : null;
			if (r == null ? selected != null : selected == null || r.Key != selected.Key) ShowRun(r);
		}

		void ShowProgress() {
			int done, total;
			library.Progress(out done, out total);
			bool show = library.Busy && total > 0;
			if (show) {
				progress.Maximum = total;
				progress.Value = Math.Min(done, total);
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
			applying = true;
			parts.Items.Clear();
			applying = false;
			parts.Hint = "";
			notes.Visible = false;
			if (r == null) {
				title.Text = "No run selected";
				subtitle.Text = rows.Count > 0 ? "Choose a run in the list." : "";
				playersLine.Text = "";
			} else if (!r.Ok) {
				title.Text = r.Demo.Name + ".dem";
				subtitle.Text = "This demo can't be read";
				playersLine.Text = r.Demo.Scan.Error ?? "";
			} else {
				var s = r.Demo.Scan;
				title.Text = r.MapName;
				subtitle.Text = s.Map + "   ·   " + (s.Coop ? "Co-op" : "Single player") + "   ·   " + (r.TimeText.Length > 0 ? r.TimeText : "no time") + (s.HasTime || s.Time <= 0 ? "" : " (demo length; no run time)");
				if (!s.Coop) playersLine.Text = s.Player;
				else if (r.Partner != null) playersLine.Text = "Blue: " + s.Player + "      Orange: " + r.Partner.Scan.Player;
				else playersLine.Text = (s.Host ? "Blue: " : "Orange: ") + s.Player + "      (the partner's demo isn't in the folders)";
				WantInfo(r);
			}
			tips.SetToolTip(title, r != null ? r.Demo.Path + (r.Partner != null ? "\n" + r.Partner.Path : "") : null);
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
			applying = true;
			parts.BeginUpdate();
			parts.Items.Clear();
			if (!info.Ok) parts.Hint = "The run's parts couldn't be read (" + info.Error + ").\nIt can still be started from the beginning.";
			else if (info.Parts.Count == 0) parts.Hint = "This run isn't split into parts: it starts from the beginning.";
			else {
				parts.Hint = "";
				int want = partRun == r.Key ? partChosen : info.Parts[0].Index;
				foreach (var p in info.Parts) {
					var item = new ListViewItem(new[] { p.Index.ToString(), RouteCli.FormatTime(p.Start) + " – " + RouteCli.FormatTime(p.End), p.Name }) { Tag = p.Index };
					parts.Items.Add(item);
					if (p.Index == want) item.Selected = true;
				}
				if (parts.SelectedItems.Count == 0) parts.Items[0].Selected = true;
				if (parts.IsHandleCreated) parts.EnsureVisible(parts.SelectedIndices[0]);
			}
			parts.EndUpdate();
			applying = false;

			if (info.Ok && info.Players.Count > 0) {
				var names = new List<string>();
				foreach (var p in info.Players) {
					string team = p.Team == "blue" ? "Blue: " : p.Team == "orange" ? "Orange: " : "";
					names.Add(team + p.Name + (p.HasInputs ? "" : " (not recorded)"));
				}
				playersLine.Text = string.Join("      ", names);
			}
			notes.Text = info.Ok ? string.Join("  ", info.Notes) : "";
			notes.Visible = notes.Text.Length > 0;
			tips.SetToolTip(notes, info.Ok && info.Notes.Count > 0 ? string.Join("\n", info.Notes) : null);
			FitColumns();
		}

		int SelectedPart { get { return parts.SelectedItems.Count > 0 ? (int)parts.SelectedItems[0].Tag : 1; } }

		// ---- options ----

		void ShowOptions() {
			var o = settings.Options;
			building = true;
			(o.Watch ? watch : practise).Checked = true;
			(o.Orange ? orange : blue).Checked = true;
			(o.RealPartner ? real : bot).Checked = true;
			ghost.Checked = o.Ghost;
			clock.Checked = o.Clock;
			keys.Checked = o.Keys;
			line.Checked = o.Line;
			loop.Checked = o.Loop;
			leadIn.Value = (decimal)Math.Max(Options.MinLeadIn, Math.Min(Options.MaxLeadIn, o.LeadIn));
			speed.Value = (decimal)Math.Max(Options.MinSpeed, Math.Min(Options.MaxSpeed, o.Speed));
			extra.Text = o.Extra;
			building = false;
			EnableOptions();
		}

		void ReadOptions() {
			var o = settings.Options;
			o.Watch = watch.Checked;
			o.Orange = orange.Checked;
			o.RealPartner = real.Checked;
			o.Ghost = ghost.Checked;
			o.Clock = clock.Checked;
			o.Keys = keys.Checked;
			o.Line = line.Checked;
			o.Loop = loop.Checked;
			o.LeadIn = (double)leadIn.Value;
			o.Speed = (double)speed.Value;
			o.Extra = extra.Text.Trim();
		}

		void OptionChanged() {
			if (!building) EnableOptions();
		}

		// the co-op choices only mean something for a co-op run, and the robot only when a bot plays the other one
		void EnableOptions() {
			if (building || launch == null) return;
			bool ok = selected != null && selected.Ok, coop = ok && selected.Coop;
			partnerCaption.Enabled = bot.Enabled = real.Enabled = coop;
			roleCaption.Enabled = blue.Enabled = orange.Enabled = coop && bot.Checked;
			launch.Enabled = ok;
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
			status.ForeColor = messageIsError && message.Length > 0 ? ErrorText : SystemColors.ControlText;
			status.Text = message.Length > 0 ? message : "Choose a run, how to practise it, then Launch.";
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
			SelectInList(index, true);
			selectDelay.Stop();
			SyncSelection();
		}

		// as if the window had been dragged to a monitor with this scale
		public void RescaleTo(int percent) {
			float f = percent / 100f / S;
			Rescale(percent / 100f, new Rectangle(Left, Top, (int)(Width * f), (int)(Height * f)));
			CreateHidden(); // (a window that isn't showing doesn't create its new controls by itself)
		}

		public void SelectPart(int number) {
			foreach (ListViewItem item in parts.Items) if ((int)item.Tag == number) { item.Selected = true; item.EnsureVisible(); }
		}

		public void SavePicture(string file) {
			CreateHidden(); // (controls added since)
			FitColumns();
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
				selectDelay.Dispose();
				if (tips != null) tips.Dispose();
				foreach (var f in new[] { uiFont, titleFont, launchFont }) if (f != null) f.Dispose();
			}
			base.Dispose(disposing);
		}
	}
}
