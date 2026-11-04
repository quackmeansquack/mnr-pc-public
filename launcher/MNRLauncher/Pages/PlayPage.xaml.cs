using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using MNRLauncher.Core;

namespace MNRLauncher.Pages;

public partial class PlayPage : UserControl, IRefresh
{
    readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromSeconds(1) };

    public PlayPage()
    {
        InitializeComponent();
        var art = Path.Combine(Paths.GameData, "PIC1.PNG");
        if (File.Exists(art)) Art.Background = new System.Windows.Media.ImageBrush(new BitmapImage(new Uri(art))) { Stretch = System.Windows.Media.Stretch.UniformToFill, AlignmentX = System.Windows.Media.AlignmentX.Right };
        timer.Tick += (_, _) => UpdateRunning();
        timer.Start();
    }

    /// <summary>Updates the Play page UI: shows current settings, active accounts/profiles, server status,
    /// crash notifications, release notes, and game running state.</summary>
    public void Refresh()
    {
        var s = MainWindow.Settings;
        var mods = ModManager.Scan(s).Where(m => m.Enabled).ToList();
        var bits = new List<string> { "Version 1.11", s.FpsCap == 30 ? "30 fps (as on the console)" : $"{s.FpsCap} fps cap (experimental)" };
        bits.Add(mods.Count == 0 ? "no mods" : mods.Count == 1 ? "1 mod: " + mods[0].DisplayName : $"{mods.Count} mods");
        var prof = Profiles.Active(s);
        bits.Add(prof.IsDefault ? "default saves" : "save profile: " + prof.Name);
        if (s.Mute) bits.Add("muted");
        if (s.Resolution > 720) bits.Add($"{s.Resolution}p");
        Status.Text = string.Join("   ·   ", bits);
        loadingAccounts = true;
        var accounts = Accounts.List(s);
        AccountPick.ItemsSource = accounts;
        AccountPick.SelectedItem = accounts.FirstOrDefault(a => a.Active);
        AccountRow.Visibility = accounts.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        var profiles = Profiles.List(s);
        ProfilePick.ItemsSource = profiles;
        ProfilePick.SelectedItem = profiles.FirstOrDefault(p => p.Active);
        ServerPick.Items.Clear();
        foreach (var (name, _) in OnlineSettings.Servers) ServerPick.Items.Add(name);
        int si = Array.FindIndex(OnlineSettings.Servers, x => x.address == s.ServerAddress.Trim());
        if (si < 0) { ServerPick.Items.Add(s.ServerAddress.Trim()); si = ServerPick.Items.Count - 1; }
        ServerPick.SelectedIndex = si;
        loadingAccounts = false;

        var crash = BugReport.LastCrash();
        CrashCard.Visibility = crash != null && crash.Value.file != s.CrashSeen ? Visibility.Visible : Visibility.Collapsed;
        if (crash != null) CrashText.Text = $"The session of {crash.Value.when:d MMM, HH:mm} ended in a crash. A bug report gathers its log and settings (no passwords) in a zip to send.";
        var installed = Updater.InstalledRelease(Paths.Root);
        NewsCard.Visibility = s.LatestNotes.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        NewsTitle.Text = installed != null && installed != s.LatestRelease ? $"New: release {s.LatestRelease}" : $"What's new in {s.LatestRelease}";
        NewsText.Text = s.LatestNotes;
        var problem = GameRunner.Problem();
        Problem.Text = problem ?? "";
        Problem.Visibility = problem == null ? Visibility.Collapsed : Visibility.Visible;
        UpdateRunning();
    }

    bool loadingAccounts;
    /// <summary>Switches to the selected account, showing a warning if the game is running.</summary>
    void AccountPick_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (loadingAccounts || AccountPick.SelectedItem is not Account a || a.Active) return;
        if (GameRunner.IsRunning)
        {
            MessageBox.Show("Close the game first: it is signed in with the current account.", "Accounts", MessageBoxButton.OK, MessageBoxImage.Information);
            Refresh(); return;
        }
        Accounts.Use(a, MainWindow.Settings); MainWindow.Settings.Save();
        Refresh();
    }

    /// <summary>Switches to the selected save profile, showing a warning if the game is running.</summary>
    void ProfilePick_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (loadingAccounts || ProfilePick.SelectedItem is not Profile p || p.Active) return;
        if (GameRunner.IsRunning) { MessageBox.Show("Close the game first: it is using the current saves.", "Saves"); Refresh(); return; }
        SaveBackup.Active(MainWindow.Settings, "before-switch");
        MainWindow.Settings.ActiveProfile = p.IsDefault ? "" : p.Name; MainWindow.Settings.Save();
        Refresh();
    }

    /// <summary>Switches the online server address based on the selected server in the dropdown.</summary>
    void ServerPick_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (loadingAccounts || ServerPick.SelectedIndex < 0 || ServerPick.SelectedIndex >= OnlineSettings.Servers.Length) return;
        MainWindow.Settings.ServerAddress = OnlineSettings.Servers[ServerPick.SelectedIndex].address; MainWindow.Settings.Save();
    }

    /// <summary>Creates and shows a bug report zip file.</summary>
    void Report_Click(object sender, RoutedEventArgs e) => BugReport.MakeAndShow();
    /// <summary>Dismisses the crash notification by recording the crash file name.</summary>
    void Dismiss_Click(object sender, RoutedEventArgs e)
    {
        var c = BugReport.LastCrash();
        if (c != null) { MainWindow.Settings.CrashSeen = c.Value.file; MainWindow.Settings.Save(); }
        CrashCard.Visibility = Visibility.Collapsed;
    }

    /// <summary>Updates the Play button text and enabled state based on whether the game is running.</summary>
    void UpdateRunning()
    {
        bool running = GameRunner.IsRunning;
        PlayButton.Content = running ? "RUNNING" : "PLAY";
        PlayButton.IsEnabled = !running && GameRunner.Problem() == null;
    }

    /// <summary>Starts the game: synchronizes published creations with the server, checks for mod conflicts,
    /// hooks the early-exit callback, and launches the game. Shows a dialog if the game crashes right after starting.</summary>
    async void Play_Click(object sender, RoutedEventArgs e)
    {
        var s = MainWindow.Settings;
        PlayButton.IsEnabled = false; PlayButton.Content = "…";
        try
        {   // the published creations on disk made to match the server the game is about to check them with
            var notes = await CreationSync.Prepare(s, t => Dispatcher.Invoke(() => Status.Text = t));
            if (notes.Count > 0) MessageBox.Show(string.Join("\n\n", notes.Take(12)) + (notes.Count > 12 ? $"\n\n…and {notes.Count - 12} more." : ""), "Your creations on this server");
        }
        catch (Exception ex) { Status.Text = "Couldn't check your creations with the server: " + ex.Message; }
        finally { UpdateRunning(); }
        var mods = ModManager.Scan(s).Where(m => m.Enabled).OrderBy(m => s.EnabledMods.IndexOf(m.Id)).ToList();
        var conflicts = ModManager.Conflicts(mods);
        if (conflicts.Count > 0 && MessageBox.Show($"{conflicts.Count} file(s) are replaced by more than one mod; the first in load order wins:\n\n" +
                string.Join("\n", conflicts.Take(8)) + (conflicts.Count > 8 ? "\n..." : "") + "\n\nPlay anyway?", "Mod conflicts",
                MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes) return;
        HookEarlyExit();
        try { s.Save(); GameRunner.Start(s, mods); UpdateRunning(); }
        catch (Exception ex) { MessageBox.Show(ex.Message, "Couldn't start the game", MessageBoxButton.OK, MessageBoxImage.Error); }
    }

    static bool earlyHooked;
    /// <summary>Registers the early-exit callback once. Shows a dialog if the game crashes right after starting.</summary>
    void HookEarlyExit()
    {
        if (earlyHooked) return;
        earlyHooked = true;
        GameRunner.EarlyExit += msg => Dispatcher.BeginInvoke(() =>
        {
            UpdateRunning();
            MessageBox.Show(msg, "The game stopped right after starting", MessageBoxButton.OK, MessageBoxImage.Error);
        });
    }

    /// <summary>Opens the logs folder in Windows Explorer.</summary>
    void Logs_Click(object sender, RoutedEventArgs e) { Directory.CreateDirectory(Paths.LogsDir); Process.Start("explorer.exe", Paths.LogsDir); }
    /// <summary>Opens the install root folder in Windows Explorer.</summary>
    void Folder_Click(object sender, RoutedEventArgs e) => Process.Start("explorer.exe", Paths.Root);
}
