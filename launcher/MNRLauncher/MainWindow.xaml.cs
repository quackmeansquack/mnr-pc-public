using System.IO;
using System.Windows;
using System.Windows.Controls;
using MNRLauncher.Core;
using MNRLauncher.Pages;

namespace MNRLauncher;

public partial class MainWindow : Wpf.Ui.Controls.FluentWindow
{
    public static Settings Settings { get; private set; } = new();
    readonly Dictionary<RadioButton, Func<UserControl>> pages;
    readonly Dictionary<RadioButton, UserControl> made = new();
    static MainWindow? current;

    /// <summary>(public edition) Setup finished or changed what is installed: the pages that show it re-read the disk.</summary>
    public static void SetupChanged()
    {
        if (current == null) return;
        foreach (var p in current.made.Values) if (p is PlayPage pp) pp.Refresh();
    }

    /// <summary>Initializes the main window: loads settings, migrates accounts, resets old caches, configures navigation pages, and checks for updates.</summary>
    public MainWindow()
    {
        InitializeComponent();
        Paths.EnsureFolders();
        Settings = Settings.Load();
        Accounts.Migrate(Settings);   // the sign-in from before accounts existed becomes the first account
        try { CreationInstaller.FixSmallThumbnails(); } catch (Exception) { }   // 64x64 menu pictures from earlier versions
        try { CreationInstaller.FixCutNames(); } catch (Exception) { }   // names cut to 24 characters by earlier versions
        if (Settings.CompositeCacheReset < 1)
        {   // once: the game's cache of composited racer and kart textures (USRDIR\DATA\COMPOSITES\LOCAL_2) held entries
            // saved incomplete by earlier builds (kart paint as a grid, odd reflections); the game rebuilds what is missing
            try
            {
                var dir = Path.Combine(Paths.InstalledUsrdir, "DATA", "COMPOSITES", "LOCAL_2", "COMPRESSED");
                if (Directory.Exists(dir)) foreach (var f in Directory.GetFiles(dir)) File.Delete(f);
                Settings.CompositeCacheReset = 1; Settings.Save();
            }
            catch (Exception) { }
        }
        Version.Text = $"Launcher {LauncherVersion}\n{Paths.Root}";
        pages = new()
        {
            [NavPlay] = () => new PlayPage(),
            [NavCreations] = () => new TabsPage(("Browse online", () => new BrowsePage()), ("Installed and mine", () => new CreationsPage())),
            [NavMods] = () => new ModsPage(),
            [NavProfiles] = () => new TabsPage(("Accounts", () => new AccountsPage()), ("Save profiles", () => new SavesPage())),
            [NavSettings] = () => new TabsPage(("Game", () => new GameSettings()), ("Graphics", () => new GraphicsSettings()), ("Audio and voice", () => new AudioSettings()),
                                               ("Controls", () => new ControlsPage()), ("Online", () => new OnlineSettings()), ("Appearance", () => new AppearanceSettings()),
                                               ("Advanced", () => new AdvancedSettings())),
            [NavUpdate] = () => new UpdatePage(),
        };
#if PUBLIC_EDITION
        pages[NavSetup] = () => new SetupPage();   // the player's own game files, and the build made on this PC
        NavSetup.Visibility = Visibility.Visible;
        current = this;
#endif
        Updater.CleanUpOldLauncher();
#if PUBLIC_EDITION
        if (GameSetup.Complete) NavPlay.IsChecked = true; else NavSetup.IsChecked = true;   // something missing: start at Setup
#else
        if (Paths.Installed) NavPlay.IsChecked = true; else NavUpdate.IsChecked = true;   // nothing installed yet: start at the installer
#endif
        Closing += (_, _) => Settings.Save();
        Loaded += async (_, _) => await CheckForUpdatesQuietly();
    }

    /// <summary>The release the launcher was built for (set by publish_release.py), else the assembly version.</summary>
    public static string LauncherVersion
    {
        get
        {
            var asm = typeof(MainWindow).Assembly;
            var info = (Attribute.GetCustomAttribute(asm, typeof(System.Reflection.AssemblyInformationalVersionAttribute)) as System.Reflection.AssemblyInformationalVersionAttribute)?.InformationalVersion;
            if (!string.IsNullOrEmpty(info)) { var plus = info.IndexOf('+'); return plus > 0 ? info[..plus] : info; }
            return asm.GetName().Version?.ToString() ?? "?";
        }
    }

    /// <summary>At start: ask GitHub whether the game or the launcher has a newer release; if so, a button in the sidebar
    /// leads to Install &amp; updates. Silent when there is no repository/token or no connection.</summary>
    async Task CheckForUpdatesQuietly()
    {
        var a = Updater.LoadAccount();
        if (!a.Ready || !Paths.Installed) return;
        try
        {
            var m = await Updater.FetchManifest(a);
            var root = Paths.Root;
            Settings.LatestRelease = m.Version; Settings.LatestNotes = m.Notes; Settings.Save();
            if (made.TryGetValue(NavPlay, out var pp) && pp is PlayPage play && NavPlay.IsChecked == true) play.Refresh();
            // a newer release than the one installed (files the game changes while playing don't count); without a record
            // of the installed release yet (older launchers), one full check of the files decides and records it
            var installed = Updater.InstalledRelease(root);
            bool game, launcher = await Task.Run(() => Updater.LauncherOutdated(m));
            if (installed != null) game = installed != m.Version;
            else game = (await Task.Run(() => Updater.Compare(root, m))).Packs.Count > 0;
            if (!game && !launcher) return;
            var what = game && launcher ? "game and launcher" : launcher ? "launcher" : "game";
            UpdateNotice.Content = "Update available";
            UpdateNotice.ToolTip = $"New {what}: release {m.Version}" + (m.Notes.Length > 0 ? "\n" + m.Notes : "");
            UpdateNotice.Visibility = Visibility.Visible;
        }
        catch (Exception) { /* offline or GitHub unreachable: the Install & updates page can still check by hand */ }
    }

    /// <summary>Handles update notice clicks: navigates to the updates page and triggers an immediate check.</summary>
    void UpdateNotice_Click(object sender, RoutedEventArgs e)
    {
        NavUpdate.IsChecked = true;
        if (made.TryGetValue(NavUpdate, out var p) && p is UpdatePage up) up.CheckNow();
    }

    /// <summary>(tests, App --shot) each sidebar page, and each tab of the tabbed ones, drawn to a PNG in folder.</summary>
    public void SaveShots(string folder)
    {
        Directory.CreateDirectory(folder);
        File.WriteAllText(Path.Combine(folder, "theme.txt"),
            $"app theme {Wpf.Ui.Appearance.ApplicationThemeManager.GetAppTheme()}, system {Wpf.Ui.Appearance.ApplicationThemeManager.GetSystemTheme()}\n" +
            $"window bg {Background}, Bg {TryFindResource("Bg")}, ApplicationBackgroundBrush {TryFindResource("ApplicationBackgroundBrush")}, TextFillColorPrimaryBrush {TryFindResource("TextFillColorPrimaryBrush")}\n" +
            string.Join("\n", Application.Current.Resources.MergedDictionaries.Select(d => d.GetType().Name + " " + d.Source + " / " + string.Join(", ", d.MergedDictionaries.Select(x => x.Source)))));
        int n = 0;
        void Pump() => Dispatcher.Invoke(() => { }, System.Windows.Threading.DispatcherPriority.ApplicationIdle);
        void Shot(string name)
        {
            Pump(); UpdateLayout(); Pump();
            var root = (FrameworkElement)Content;
            var bmp = new System.Windows.Media.Imaging.RenderTargetBitmap((int)root.ActualWidth, (int)root.ActualHeight, 96, 96, System.Windows.Media.PixelFormats.Pbgra32);
            bmp.Render(root);
            var enc = new System.Windows.Media.Imaging.PngBitmapEncoder(); enc.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bmp));
            using var f = File.Create(Path.Combine(folder, $"{n++:00}_{name}.png")); enc.Save(f);
        }
        foreach (var rb in pages.Keys)
        {
            rb.IsChecked = true;
            if (made[rb] is TabsPage tp)
                for (int i = 0; i < tp.Count; i++)
                {
                    tp.Select(i);
                    if (tp.Current is BrowsePage bp)
                    {   // the listing and its pictures come from the internet: give them a moment, and open the first
                        void Wait(int ms) { var end = DateTime.Now.AddMilliseconds(ms); while (DateTime.Now < end) { Pump(); System.Threading.Thread.Sleep(50); } }
                        for (int t = 0; t < 100 && bp.List.Items.Count == 0; t++) Wait(100);
                        bp.List.SelectedIndex = 0; Wait(3000);
                    }
                    Shot($"{rb.Content}_{i}");
                }
            else Shot($"{rb.Content}");
        }
    }

    /// <summary>Handles navigation radio button clicks: lazily creates page instances, refreshes them, and displays the selected page.</summary>
    void Nav_Checked(object sender, RoutedEventArgs e)
    {
        var rb = (RadioButton)sender;
        if (pages == null) return;
        if (!made.TryGetValue(rb, out var page)) made[rb] = page = pages[rb]();
        if (page is IRefresh r) r.Refresh();   // pages show the current state of the disk each time
        Page.Content = page;
    }
}

/// <summary>Pages that re-read their data when shown.</summary>
public interface IRefresh { void Refresh(); }
