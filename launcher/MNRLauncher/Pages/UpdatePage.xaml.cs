using System;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using MNRLauncher.Core;

namespace MNRLauncher.Pages;

public partial class UpdatePage : UserControl, IRefresh
{
    Updater.Plan? plan;
    CancellationTokenSource? cts;

    public UpdatePage()
    {
        InitializeComponent();
#if PUBLIC_EDITION
        Intro.Text = "The PC runtime's source and this launcher come from the releases of the public repository " + Updater.PublicRepo + ". After an update the game is rebuilt on this PC (Setup). The game itself comes from your own files (Setup).";
        Repo.IsReadOnly = true;
        TokenState.Visibility = Visibility.Collapsed;
        TokenHelp.Text = "The access token is only needed while the repository is private (testing): a fine-grained token with Contents: Read-only on it. Leave it empty once the repository is public.";
#endif
    }

    public void Refresh()
    {
        if (cts != null) return;   // busy
        var a = Updater.LoadAccount();
        Repo.Text = a.Repo;
        Token.Password = "";
        TokenState.Text = a.TokenProtected.Length > 0 ? "A token is saved (leave the box empty to keep it)." : "No token saved yet.";
        Folder.Text = Paths.Root;
        FolderState.Text = Paths.Installed ? "The game is installed here." : "Nothing installed here yet.";
    }

    Updater.Account? SaveAccount()
    {
        var a = Updater.LoadAccount();
        a.Repo = Repo.Text.Trim().Trim('/');
        if (a.Repo.StartsWith("https://github.com/", StringComparison.OrdinalIgnoreCase)) a.Repo = a.Repo[19..];
        if (Token.Password.Length > 0) { a.Token = Token.Password.Trim(); Token.Password = ""; }
        Updater.SaveAccount(a);
        TokenState.Text = a.TokenProtected.Length > 0 ? "A token is saved (leave the box empty to keep it)." : "No token saved yet.";
        if (!a.Ready) { Status.Text = "Enter the repository (owner/name) and an access token."; return null; }
        return a;
    }

    /// <summary>A different install folder: remembered, then the launcher restarts there.</summary>
    bool FolderChanged()
    {
        var want = Folder.Text.Trim();
        if (want.Length == 0) return false;
        if (string.Equals(Path.GetFullPath(want).TrimEnd('\\'), Paths.Root.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase)) return false;
        Directory.CreateDirectory(want);
        Paths.SetChosenRoot(want);
        MessageBox.Show($"The install folder is now\n{Path.GetFullPath(want)}\n\nThe launcher restarts to use it; then press Check for updates again.", "Install folder", MessageBoxButton.OK, MessageBoxImage.Information);
        Updater.Restart();
        return true;
    }

    void Browse_Click(object sender, RoutedEventArgs e)
    {
        var d = new Microsoft.Win32.OpenFolderDialog { Title = "Install folder (about 17 GB)" };
        if (d.ShowDialog() == true) Folder.Text = Path.Combine(d.FolderName, d.FolderName.EndsWith("MNR", StringComparison.OrdinalIgnoreCase) ? "" : "MNR");
    }

    /// <summary>Starts a check, as the button does (the sidebar's "Update available" notice).</summary>
    public void CheckNow() { if (cts == null) Check_Click(this, new RoutedEventArgs()); }

    async void Check_Click(object sender, RoutedEventArgs e)
    {
        if (FolderChanged()) return;
        var a = SaveAccount();
        if (a == null) return;
        Busy(true); ApplyButton.IsEnabled = false; plan = null;
        try
        {
            Status.Text = "Asking GitHub for the latest release...";
            var m = await Updater.FetchManifest(a, cts!.Token);
            var root = Paths.Root;
            var p = await Task.Run(() => Updater.Compare(root, m, s => Dispatcher.BeginInvoke(() => Status.Text = s)));
            plan = p;
            Notes.Text = m.Notes.Length > 0 ? $"Release {m.Version}: {m.Notes}" : $"Release {m.Version}";
            if (p.UpToDate) Status.Text = "Everything is up to date.";
            else
            {
                var parts = p.Packs.Select(x => x.Name).ToList();
                if (p.LauncherUpdate) parts.Add("launcher");
                Status.Text = $"{(Paths.Installed ? "Update" : "Install")}: {p.DownloadBytes / 1e9 + (p.LauncherUpdate ? m.Launcher!.Size / 1e9 : 0):0.00} GB to download ({string.Join(", ", parts)}).";
                ApplyButton.IsEnabled = true;
            }
        }
        catch (OperationCanceledException) { Status.Text = "Cancelled."; }
        catch (Exception ex) { Status.Text = ex.Message; }
        finally { Busy(false); }
    }

    async void Apply_Click(object sender, RoutedEventArgs e)
    {
        var a = SaveAccount();
        if (a == null || plan == null) return;
        if (GameRunner.IsRunning) { Status.Text = "Close the game first."; return; }
        Busy(true); ApplyButton.IsEnabled = false;
        var p = plan;
        try
        {
            var root = Paths.Root;
            var settings = MainWindow.Settings;
            if (Paths.Installed) { Status.Text = "Backing up your saves…"; await Task.Run(() => SaveBackup.Active(settings, "before-update")); }
            await Task.Run(() => Updater.Apply(root, a, p, (done, total, text) => Dispatcher.BeginInvoke(() =>
            {
                Bar.Value = total > 0 ? 1000.0 * done / total : 0; Status.Text = text;
            }), cts!.Token));
            plan = null;
            if (p.LauncherUpdate)
            {
                MessageBox.Show("The launcher was updated and restarts now.", "Updated", MessageBoxButton.OK, MessageBoxImage.Information);
                Updater.Restart();
                return;
            }
            Status.Text = "Installed and up to date.";
#if PUBLIC_EDITION
            if (p.Packs.Count > 0 && GameSetup.DiscDone && GameSetup.UpdateDone)
            {   // new runtime source: the game is built again (only what changed is recompiled)
                await GameSetup.EnsureTools((t, f) => Dispatcher.BeginInvoke(() => Status.Text = t), cts!.Token);
                await GameSetup.Build((t, f) => Dispatcher.BeginInvoke(() => { Status.Text = t; if (f >= 0) Bar.Value = 1000 * f; }), _ => { }, cts!.Token);
                Status.Text = "Updated and rebuilt.";
                MainWindow.SetupChanged();
            }
#endif
            FolderState.Text = Paths.Installed ? "The game is installed here." : "Nothing installed here yet.";
        }
        catch (OperationCanceledException) { Status.Text = "Cancelled (finished parts are kept; Check for updates continues)."; }
        catch (Exception ex) { Status.Text = ex.Message; }
        finally { Busy(false); }
    }

    void Cancel_Click(object sender, RoutedEventArgs e) => cts?.Cancel();

    /// <summary>Installs the player's own DLC packages (PkgInstaller): their license files go into the game's folder.</summary>
    async void InstallPkg_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog { Filter = "PS3 packages (*.pkg)|*.pkg", Multiselect = true, Title = "Install DLC packages" };
        if (dlg.ShowDialog() != true) return;
        var lines = new System.Collections.Generic.List<string>();
        foreach (var f in dlg.FileNames)
        {
            try
            {
                var id = PkgInstaller.ContentId(f);
                if (!id.Contains("BCUS98167") && !id.Contains("BCES00701") && !id.Contains("BCES00764") && !id.Contains("BCAS20112") && !id.Contains("BCJS30040"))
                { lines.Add($"{Path.GetFileName(f)}: not a ModNation Racers package ({id})."); continue; }
                if (!await Task.Run(() => PkgInstaller.IsDlc(f)))
                { lines.Add($"{Path.GetFileName(f)}: not DLC (no licenses); a mod package goes in with Import on the Mods page."); continue; }
                PkgStatus.Text = $"Installing {id}...";
                var r = await Task.Run(() => PkgInstaller.Install(f, Paths.Hdd0, force: true));
                lines.Add($"{r.ContentId}: {(r.Files > 0 ? $"{r.Files} file(s) added" : "already installed")}.");
            }
            catch (Exception ex) { lines.Add($"{Path.GetFileName(f)}: {ex.Message}"); }
        }
        PkgStatus.Text = string.Join("\n", lines);
    }

    void Busy(bool on)
    {
        if (on) cts = new CancellationTokenSource(); else { cts?.Dispose(); cts = null; }
        CheckButton.IsEnabled = !on;
        CancelButton.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        Bar.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        if (on) Bar.Value = 0;
    }
}
