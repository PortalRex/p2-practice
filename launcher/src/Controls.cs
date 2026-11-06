// The few things the stock controls don't do: an Explorer-style list with a sort arrow and a line of text for
// when it is empty, grey prompt text in a text box, and the monitor's scale.
using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace P2Practice {
	class ListViewEx : ListView {
		readonly Label hint; // shown across the list when there is nothing in it

		// The list exists as a window, with its columns and rows in: selection and the sort arrow can be set from now.
		public event EventHandler Realized;

		public ListViewEx() {
			View = View.Details;
			FullRowSelect = true;
			HideSelection = false;
			MultiSelect = false;
			DoubleBuffered = true;
			hint = new Label { AutoSize = false, TextAlign = ContentAlignment.TopCenter, UseMnemonic = false, Visible = false };
			Controls.Add(hint);
		}

		public string Hint {
			get { return hint.Text; }
			set {
				hint.Text = value ?? "";
				hint.Visible = hint.Text.Length > 0;
				PlaceHint();
			}
		}

		void PlaceHint() {
			if (hint == null || hint.Text.Length == 0) return;
			hint.BackColor = BackColor;
			hint.ForeColor = SystemColors.GrayText;
			int line = Font.Height, pad = line; // below the column headers, with room at the sides
			hint.SetBounds(pad, line * 3 + line / 2, Math.Max(10, ClientSize.Width - pad * 2), Math.Max(line, Math.Min(line * 4, ClientSize.Height - line * 4)));
		}

		protected override void OnHandleCreated(EventArgs e) {
			base.OnHandleCreated(e);
			Native.SetWindowTheme(Handle, "explorer", null); // rows highlight as in Explorer
			PlaceHint();
			if (Realized != null) Realized(this, EventArgs.Empty);
		}
		protected override void OnResize(EventArgs e) { base.OnResize(e); PlaceHint(); }
		protected override void OnFontChanged(EventArgs e) { base.OnFontChanged(e); PlaceHint(); }

		// the little arrow in the header of the column the list is sorted by (-1: none)
		public void ShowSortArrow(int column, bool descending) {
			if (!IsHandleCreated) return;
			IntPtr header = Native.SendMessage(Handle, 0x101F /* LVM_GETHEADER */, IntPtr.Zero, IntPtr.Zero);
			if (header == IntPtr.Zero) return;
			for (int i = 0; i < Columns.Count; i++) {
				var item = new Native.HDITEM { mask = 0x0004 /* HDI_FORMAT */ };
				Native.SendMessage(header, 0x120B /* HDM_GETITEMW */, (IntPtr)i, ref item);
				item.fmt &= ~(0x0400 | 0x0200); // HDF_SORTUP, HDF_SORTDOWN
				if (i == column) item.fmt |= descending ? 0x0200 : 0x0400;
				Native.SendMessage(header, 0x120C /* HDM_SETITEMW */, (IntPtr)i, ref item);
			}
		}
	}

	static class Native {
		[StructLayout(LayoutKind.Sequential)]
		public struct HDITEM {
			public uint mask;
			public int cxy;
			public IntPtr pszText, hbm;
			public int cchTextMax, fmt;
			public IntPtr lParam;
			public int iImage, iOrder;
			public uint type;
			public IntPtr pvFilter;
			public uint state;
		}

		[StructLayout(LayoutKind.Sequential)]
		public struct RECT { public int Left, Top, Right, Bottom; }

		[DllImport("uxtheme.dll", CharSet = CharSet.Unicode)] public static extern int SetWindowTheme(IntPtr window, string app, string idList);
		[DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr window, int msg, IntPtr wParam, IntPtr lParam);
		[DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr window, int msg, IntPtr wParam, ref HDITEM item);
		[DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr SendMessage(IntPtr window, int msg, IntPtr wParam, string text);
		[DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr window);

		// grey text shown in an empty text box
		public static void SetPrompt(TextBox box, string text) {
			Action set = () => SendMessage(box.Handle, 0x1501 /* EM_SETCUEBANNER */, (IntPtr)1, text);
			if (box.IsHandleCreated) set();
			box.HandleCreated += (s, e) => set();
		}

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
