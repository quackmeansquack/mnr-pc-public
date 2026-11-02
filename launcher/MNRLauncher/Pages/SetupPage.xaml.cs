using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using MNRLauncher.Core;

namespace MNRLauncher.Pages;

/// <summary>(public edition) Where the game comes from: the player points the launcher at their disc (image or folder),
/// their 1.11 update package and, for voice chat when their disc
/// copy lacks it, a PS3 system update file; Set up then runs GameSetup's steps. Shown at start while anything is missing.</summary>
public partial class SetupPage : UserControl, IRefresh
{
    CancellationTokenSource? cts;

    public SetupPage() { InitializeComponent(); }

    public void Refresh()
    {
        if (cts != null) return;
        DiscState.Text = GameSetup.DiscDone ? "✓ The disc files are copied." : "Not set up yet.";
        UpdateState.Text = GameSetup.UpdateDone ? "✓ The 1.11 update is installed." : "Not installed yet.";
        VoiceState.Text = GameSetup.VoiceReady ? "✓ Found: voice chat will work." : "Not found (the game still runs; voice chat stays off until a system update file is given).";
        SetupButton.Content = GameSetup.Complete ? "Set up (everything is ready)" : "Set up";
        RebuildButton.IsEnabled = GameSetup.DiscDone && GameSetup.UpdateDone;
        Status.Text = GameSetup.Complete ? "The game is ready to play." : "";
    }

    void PickIso_Click(object sender, RoutedEventArgs e)
    {
        var d = new Microsoft.Win32.OpenFileDialog { Filter = "Disc images (*.iso)|*.iso|All files|*.*", Title = "Your ModNation Racers disc image" };
        if (d.ShowDialog() == true) DiscSource.Text = d.FileName;
    }
    void PickDiscFolder_Click(object sender, RoutedEventArgs e)
    {
        var d = new Microsoft.Win32.OpenFolderDialog { Title = "The game folder from your disc dump (containing PS3_GAME)" };
        if (d.ShowDialog() == true) DiscSource.Text = d.FolderName;
    }
    void PickPkg_Click(object sender, RoutedEventArgs e)
    {
        var d = new Microsoft.Win32.OpenFileDialog { Filter = "PS3 packages (*.pkg)|*.pkg", Title = "The ModNation Racers 1.11 update package" };
        if (d.ShowDialog() == true) UpdateSource.Text = d.FileName;
    }
    void PickPup_Click(object sender, RoutedEventArgs e)
    {
        var d = new Microsoft.Win32.OpenFileDialog { Filter = "PS3 system update (PS3UPDAT.PUP)|*.PUP|All files|*.*", Title = "A PS3 system update file" };
        if (d.ShowDialog() == true) PupSource.Text = d.FileName;
    }

    void Report(string text, double fraction) => Dispatcher.BeginInvoke(() =>
    {
        Status.Text = text;
        Bar.IsIndeterminate = fraction < 0;
        if (fraction >= 0) Bar.Value = 1000 * Math.Clamp(fraction, 0, 1);
    });
    void AddLog(string line) => Dispatcher.BeginInvoke(() => Log.Text = (Log.Text.Length > 0 ? Log.Text + "\n" : "") + line);

    async void Setup_Click(object sender, RoutedEventArgs e)
    {
        if (GameRunner.IsRunning) { Status.Text = "Close the game first."; return; }
        var disc = DiscSource.Text.Trim(); var pkg = UpdateSource.Text.Trim(); var pup = PupSource.Text.Trim();
        if (!GameSetup.DiscDone && disc.Length == 0) { Status.Text = "Pick your disc image or game folder first (1)."; return; }
        if (!GameSetup.UpdateDone && pkg.Length == 0) { Status.Text = "Pick your 1.11 update package first (2)."; return; }
        if (!GameSetup.Complete && GameSetup.FreeBytes() < GameSetup.NeedBytes - GameSetup.BytesAlreadyThere())
        { Status.Text = $"Not enough free space on this drive: about {GameSetup.NeedBytes / 1e9:0} GB is needed (Install folder on the Updates page)."; return; }
        Busy(true); Log.Text = "";
        try
        {
            if (pup.Length > 0) { await Task.Run(() => GameSetup.UseSystemUpdate(pup)); AddLog("System update file copied (voice chat)."); }
            if (!GameSetup.RuntimePresent) { await GameSetup.FetchRuntime(Report, cts!.Token); AddLog("PC runtime downloaded."); }
            await GameSetup.InstallDisc(disc, Report, cts!.Token); AddLog("Disc files copied.");
            await GameSetup.InstallUpdate(pkg, Report, cts.Token); AddLog("1.11 update installed.");
            await GameSetup.EnsureTools(Report, cts.Token); AddLog("Compiler ready.");
            if (!GameSetup.BuildDone) { await GameSetup.Build(Report, AddLog, cts.Token); }
            Report("The game is ready to play.", 1);
        }
        catch (OperationCanceledException) { Status.Text = "Cancelled. Set up continues where it stopped."; }
        catch (Exception ex) { Status.Text = ex.Message; }
        finally { Busy(false); Refresh(); if (GameSetup.Complete) Status.Text = "The game is ready to play."; MainWindow.SetupChanged(); }
    }

    async void Rebuild_Click(object sender, RoutedEventArgs e)
    {
        if (GameRunner.IsRunning) { Status.Text = "Close the game first."; return; }
        Busy(true); Log.Text = "";
        try
        {
            await GameSetup.EnsureTools(Report, cts!.Token);
            try { File.Delete(Paths.GameExe); } catch (IOException) { }
            await GameSetup.Build(Report, AddLog, cts.Token);
        }
        catch (OperationCanceledException) { Status.Text = "Cancelled."; }
        catch (Exception ex) { Status.Text = ex.Message; }
        finally { Busy(false); Refresh(); MainWindow.SetupChanged(); }
    }

    void Cancel_Click(object sender, RoutedEventArgs e) => cts?.Cancel();

    void Busy(bool on)
    {
        if (on) cts = new CancellationTokenSource(); else { cts?.Dispose(); cts = null; }
        SetupButton.IsEnabled = RebuildButton.IsEnabled = !on;
        CancelButton.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        Bar.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
        if (on) { Bar.Value = 0; Bar.IsIndeterminate = true; }
    }
}
