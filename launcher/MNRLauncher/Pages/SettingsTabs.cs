using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using MNRLauncher.Core;
using ToggleSwitch = Wpf.Ui.Controls.ToggleSwitch;

namespace MNRLauncher.Pages;

/// <summary>A settings tab: a heading and rows of cards (title and description on the left, the control on the right),
/// as WPF UI lays out settings. Every change is saved at once; Refresh re-reads the values.</summary>
public abstract class SettingsSection : UserControl, IRefresh
{
    protected readonly StackPanel Body = new() { Margin = new Thickness(8, 12, 24, 24), MaxWidth = 860, HorizontalAlignment = HorizontalAlignment.Left };
    protected static Settings S => MainWindow.Settings;
    protected bool Loading;

    protected SettingsSection() { Content = new ScrollViewer { VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Content = Body }; }

    public void Refresh() { Loading = true; Body.Children.Clear(); Build(); Loading = false; }
    protected abstract void Build();
    protected void Save() { if (!Loading) S.Save(); }

    protected TextBlock Sub(string text) => new() { Text = text, Style = (Style)FindResource("Sub") };
    protected void Heading(string text) => Body.Children.Add(new TextBlock { Text = text.ToUpperInvariant(), Style = (Style)FindResource("H2") });
    protected void Note(string text) => Body.Children.Add(new TextBlock { Text = text, Style = (Style)FindResource("Sub"), Margin = new Thickness(2, 0, 0, 10) });

    protected Border Card(string title, string? description, UIElement? control, UIElement? below = null)
    {
        var g = new Grid();
        g.ColumnDefinitions.Add(new ColumnDefinition()); g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var left = new StackPanel { VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 16, 0) };
        left.Children.Add(new TextBlock { Text = title, FontSize = 14, TextWrapping = TextWrapping.Wrap });
        if (!string.IsNullOrEmpty(description)) { var d = Sub(description); d.Margin = new Thickness(0, 2, 0, 0); left.Children.Add(d); }
        g.Children.Add(left);
        if (control is FrameworkElement fe) { fe.VerticalAlignment = VerticalAlignment.Center; Grid.SetColumn(fe, 1); g.Children.Add(fe); }
        var content = new StackPanel(); content.Children.Add(g);
        if (below != null) content.Children.Add(below);
        var b = new Border { Style = (Style)FindResource("Card"), Child = content };
        Body.Children.Add(b);
        return b;
    }

    protected ToggleSwitch Toggle(bool value, Action<bool> set)
    {
        var t = new ToggleSwitch { IsChecked = value };
        t.Click += (_, _) => { set(t.IsChecked == true); Save(); };
        return t;
    }
    protected ComboBox Choice<T>(IEnumerable<(string label, T value)> items, T current, Action<T> set, double width = 220)
    {
        var list = items.ToList();
        var c = new ComboBox { MinWidth = width };
        foreach (var (label, _) in list) c.Items.Add(label);
        c.SelectedIndex = Math.Max(0, list.FindIndex(i => Equals(i.value, current)));
        c.SelectionChanged += (_, _) => { if (c.SelectedIndex >= 0) { set(list[c.SelectedIndex].value); Save(); } };
        return c;
    }
    protected TextBox Text(string value, Action<string> set, double width = 300)
    {
        var t = new TextBox { Text = value, Width = width };
        t.LostFocus += (_, _) => { set(t.Text.Trim()); Save(); };
        return t;
    }
    protected static Button Btn(string text, Action click, bool primary = false)
    {
        var b = new Button { Content = text };
        if (primary) b.Style = (Style)Application.Current.Resources["Primary"];
        b.Click += (_, _) => click();
        return b;
    }
    protected static StackPanel Row(params UIElement[] items)
    {
        var p = new StackPanel { Orientation = Orientation.Horizontal };
        foreach (var i in items) p.Children.Add(i);
        return p;
    }
}

public class GameSettings : SettingsSection
{
    protected override void Build()
    {
        Heading("Game");
        Card("Frame rate", "30 is the console's; 60 is experimental (some menus and physics may run fast).",
             Choice(new[] { ("30 fps", 30), ("60 fps (experimental)", 60) }, S.FpsCap <= 30 ? 30 : 60, v => S.FpsCap = v));
        Card("Show the frame rate", "In the window title.", Toggle(S.ShowFps, v => S.ShowFps = v));
        Card("Use a game controller", "Xbox layout: A = Cross, B = Circle, X = Square, Y = Triangle. Wheels and other devices: the Controls tab.",
             Toggle(S.UseController, v => S.UseController = v));
        Card("Unlock all career tracks", "Any track an online room picks will load; a save keeps them unlocked once it saves.",
             Toggle(S.UnlockAllTracks, v => S.UnlockAllTracks = v));
        Card("Allow remixing downloaded creations", "Open them in the studios and publish them to the server, for online races.",
             Toggle(S.AllowRemix, v => S.AllowRemix = v));
    }
}

public class GraphicsSettings : SettingsSection
{
    protected override void Build()
    {
        Heading("Graphics");
        Card("Resolution", "The 3D scene and HUD are drawn at this size and fitted to the window. Above your screen's size it works as anti-aliasing. Higher needs more from the graphics card.",
             Choice(new[] { ("720p (as on the console)", 720), ("1080p", 1080), ("1440p", 1440), ("4K (2160p)", 2160) }, S.Resolution, v => S.Resolution = v));
        Card("Anti-aliasing (FXAA)", "Smooths jagged edges, at almost no cost. Text in the HUD gets slightly softer.", Toggle(S.Fxaa, v => S.Fxaa = v));
        Card("Texture filtering", "Anisotropic filtering keeps roads and ground sharp at a distance.",
             Choice(new[] { ("Off (as on the console)", 0), ("2x", 2), ("4x", 4), ("8x", 8), ("16x", 16) }, S.Anisotropy, v => S.Anisotropy = v));
        Note("Changes apply the next time the game starts.");
    }
}

public class AudioSettings : SettingsSection
{
    protected override void Build()
    {
        Heading("Sound");
        Card("Mute the game", null, Toggle(S.Mute, v => S.Mute = v));
        var vol = new Slider { Minimum = 0, Maximum = 100, Width = 220, Value = PcSettings.GetInt("audio", "volume", 100), IsSnapToTickEnabled = true, TickFrequency = 5 };
        var volText = new TextBlock { Width = 44, TextAlignment = TextAlignment.Right, VerticalAlignment = VerticalAlignment.Center, Text = $"{vol.Value:0}%" };
        vol.ValueChanged += (_, _) => { volText.Text = $"{vol.Value:0}%"; PcSettings.Set("audio", "volume", ((int)vol.Value).ToString()); };
        Card("Game volume", "Also - and = in the game.", Row(vol, volText));

        Heading("Voice chat");
        var (state, detail) = PcSettings.VoiceCodec();
        var status = new TextBlock { Text = state == "ready" ? "Ready" : state == "found" ? "Ready next start" : "Needs the update file", FontWeight = FontWeights.SemiBold,
                                     Foreground = (Brush)FindResource(state == "missing" ? "Warn" : "Good") };
        var pick = Btn("Pick PS3UPDAT.PUP…", PickUpdateFile);
        var codecRow = Row(status); if (state == "missing") { pick.Margin = new Thickness(12, 0, 0, 0); codecRow.Children.Add(pick); }
        Card("Voice codec", detail, codecRow);

        var mics = PcSettings.Microphones();
        var cur = PcSettings.Get("voice", "mic", "");
        var items = new List<(string, string)> { ("Windows default", "") };
        items.AddRange(mics.Select(m => (m, m)));
        Card("Microphone", "F8 in the game also switches to the next one.",
             Choice(items, mics.FirstOrDefault(m => cur.Length > 0 && m.Contains(cur)) ?? "", v => PcSettings.Set("voice", "mic", v), 280));
        Card("Push to talk", "Off: the microphone sends whenever you speak.",
             Toggle(PcSettings.GetInt("voice", "push_to_talk", 1) != 0, v => PcSettings.Set("voice", "push_to_talk", v ? "1" : "0")));
        var key = PcSettings.Get("voice", "ptt_key", "T");
        Card("Push-to-talk key", "A letter, digit or F-key.",
             Choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789".Select(ch => (ch.ToString(), ch.ToString()))
                   .Concat(Enumerable.Range(1, 12).Select(i => ($"F{i}", $"F{i}"))), key.ToUpperInvariant(), v => PcSettings.Set("voice", "ptt_key", v), 90));
        var boost = new Slider { Minimum = 50, Maximum = 400, Width = 220, Value = PcSettings.GetInt("voice", "mic_boost", 150), IsSnapToTickEnabled = true, TickFrequency = 10 };
        var boostText = new TextBlock { Width = 44, TextAlignment = TextAlignment.Right, VerticalAlignment = VerticalAlignment.Center, Text = $"{boost.Value / 100:0.0}x" };
        boost.ValueChanged += (_, _) => { boostText.Text = $"{boost.Value / 100:0.0}x"; PcSettings.Set("voice", "mic_boost", ((int)boost.Value).ToString()); };
        Card("Microphone boost", "How loud others hear you. 1.5x suits most microphones.", Row(boost, boostText));
    }

    void PickUpdateFile()
    {
        var dlg = new Microsoft.Win32.OpenFileDialog { Title = "The PS3 system update (PS3UPDAT.PUP)", Filter = "PS3 update|PS3UPDAT.PUP|All files|*.*" };
        if (dlg.ShowDialog() != true) return;
        try { File.Copy(dlg.FileName, Path.Combine(Paths.Root, "PS3UPDAT.PUP"), true); }
        catch (Exception ex) { MessageBox.Show(ex.Message, "Couldn't copy the update file"); }
        Refresh();
    }
}

public class OnlineSettings : SettingsSection
{
#if PUBLIC_EDITION
    public static readonly (string name, string address)[] Servers = { ("MNR PC", "mnr.quackcraft.net") };   // (any other: "Other…")
#else
    public static readonly (string name, string address)[] Servers = { ("MNR PC", "mnr.quackcraft.net"), ("MNR Revival", "45.32.179.33") };
#endif

    protected override void Build()
    {
        Heading("Online");
        Card("Play online", "The game signs in with the account chosen on the Profiles page.", Toggle(S.OnlineEnabled, v => S.OnlineEnabled = v));
        var known = Servers.Any(x => x.address == S.ServerAddress.Trim());
        var custom = Text(known ? "" : S.ServerAddress, v => { if (v.Length > 0) S.ServerAddress = v; }, 200);
        custom.Visibility = known ? Visibility.Collapsed : Visibility.Visible;
        var status = new TextBlock { Style = (Style)FindResource("Sub"), Margin = new Thickness(0, 6, 0, 0) };
        var pick = Choice(Servers.Select(x => ($"{x.name} ({x.address})", x.address)).Append(("Other…", "")), known ? S.ServerAddress.Trim() : "",
                          v => { custom.Visibility = v.Length == 0 ? Visibility.Visible : Visibility.Collapsed; if (v.Length > 0) S.ServerAddress = v; _ = Check(status); }, 260);
        Card("Server", "Where the game goes online. Everyone you race with must be on the same one.", Row(pick, custom), status);
        _ = Check(status);
        var port = Text((S.RacePort > 0 ? S.RacePort : 3658).ToString(), v => { if (int.TryParse(v, out var p) && p >= 1024 && p <= 65535) S.RacePort = p; }, 90);
        Card("Race port (UDP)", "3658, as on a PS3. Two PCs on the same network playing at once each need their own (3658 and 3659).", port);
        Card("Creation browser site", "The site the Creations page browses.", Text(S.BrowserUrl, v => { if (Uri.TryCreate(v, UriKind.Absolute, out _)) S.BrowserUrl = v; }, 300));
    }

    /// <summary>Whether the server's web side answers, and whether it takes worn creations (auto-share).</summary>
    static async Task Check(TextBlock status)
    {
        var addr = MainWindow.Settings.ServerAddress.Trim();
        var port = MainWindow.Settings.ServerPort > 0 ? MainWindow.Settings.ServerPort : 10050;
        status.Text = $"Checking {addr}…"; status.Foreground = (Brush)status.FindResource("SubText");
        try
        {
            using var h = new HttpClient { Timeout = TimeSpan.FromSeconds(6) };
            using var r = await h.GetAsync($"http://{addr}:{port}/mnrpc/caps");
            var share = r.IsSuccessStatusCode && (await r.Content.ReadAsStringAsync()).StartsWith("mnrpc adopt");
            status.Text = share ? "Online · characters and karts you wear are shared automatically" : "Online";
            status.Foreground = (Brush)status.FindResource("Good");
        }
        catch (Exception) { status.Text = "Can't reach the server right now"; status.Foreground = (Brush)status.FindResource("Warn"); }
    }
}

public class AppearanceSettings : SettingsSection
{
    protected override void Build()
    {
        Heading("Theme");
        Note("Colours from the game's own screens. The change shows at once.");
        var wrap = new WrapPanel();
        foreach (var t in Themes.All)
        {
            var sw = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 8, 0, 0) };
            foreach (var c in new[] { t.Bg, t.PanelHi, t.Accent, t.Accent2 })
                sw.Children.Add(new Border { Width = 22, Height = 22, CornerRadius = new CornerRadius(4), Margin = new Thickness(0, 0, 6, 0), BorderThickness = new Thickness(1),
                                             BorderBrush = (Brush)FindResource("Line"), Background = new SolidColorBrush((Color)ColorConverter.ConvertFromString(c)) });
            var inner = new StackPanel();
            inner.Children.Add(new TextBlock { Text = t.Name, FontSize = 15, FontWeight = FontWeights.SemiBold });
            inner.Children.Add(new TextBlock { Text = "From " + t.From, Style = (Style)FindResource("Sub"), Width = 220 });
            inner.Children.Add(sw);
            bool on = t.Name == Themes.Get(S.Theme).Name;
            var card = new Border { Style = (Style)FindResource("Card"), Width = 262, Margin = new Thickness(0, 0, 10, 10), Cursor = System.Windows.Input.Cursors.Hand, Child = inner,
                                    BorderThickness = new Thickness(on ? 2 : 1) };
            if (on) card.SetResourceReference(Border.BorderBrushProperty, "Accent");
            var name = t.Name;
            card.MouseLeftButtonUp += (_, _) => { S.Theme = name; S.Save(); Themes.Apply(name); Refresh(); };
            wrap.Children.Add(card);
        }
        Body.Children.Add(wrap);
    }
}

public class AdvancedSettings : SettingsSection
{
    protected override void Build()
    {
        Heading("Logs and bug reports");
        Card("Show the log window while playing", "Otherwise the log goes to logs\\last_run.txt.", Toggle(S.KeepConsole, v => S.KeepConsole = v));
        Card("Detailed log", "The game's own messages and saves, for bug reports.", Toggle(S.DetailedLog, v => S.DetailedLog = v));
        Card("Bug report", "Zips the recent logs and settings (no passwords or tokens) for you to send.",
             Row(Btn("Make bug report", () => BugReport.MakeAndShow()), Btn("Open logs folder", () => Process.Start("explorer.exe", Paths.LogsDir))));
        Heading("Switches");
        var extra = new TextBox { Text = S.ExtraEnvironment, AcceptsReturn = true, Height = 110, TextWrapping = TextWrapping.NoWrap, FontFamily = new FontFamily("Consolas"),
                                  VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Margin = new Thickness(0, 8, 0, 0) };
        extra.LostFocus += (_, _) => { S.ExtraEnvironment = extra.Text; Save(); };
        Card("Extra environment switches", "For mnr.exe: NAME=value, one per line (see re\\recomp\\README.md). These override the settings above.", null, extra);
    }
}
