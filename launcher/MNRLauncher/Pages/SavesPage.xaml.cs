using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using MNRLauncher.Core;

namespace MNRLauncher.Pages;

public partial class SavesPage : UserControl, IRefresh
{
    public SavesPage() { InitializeComponent(); }

    public void Refresh()
    {
        var keep = (List.SelectedItem as Profile)?.Folder;
        var list = Profiles.List(MainWindow.Settings);
        List.ItemsSource = list;
        List.SelectedItem = list.FirstOrDefault(p => p.Folder == keep) ?? list.First(p => p.Active);
    }

    /// <summary>Shows a warning dialog and returns true if the game is currently running.</summary>
    bool GameRunningWarning()
    {
        if (!GameRunner.IsRunning) return false;
        MessageBox.Show("Close the game first: it is using the current profile.", "Save profiles", MessageBoxButton.OK, MessageBoxImage.Information);
        return true;
    }

    void Use_Click(object sender, RoutedEventArgs e)
    {
        if (List.SelectedItem is not Profile p || GameRunningWarning()) return;
        if (!p.Active) SaveBackup.Active(MainWindow.Settings, "before-switch");
        MainWindow.Settings.ActiveProfile = p.Name; MainWindow.Settings.Save();
        Refresh();
    }

    void New_Click(object sender, RoutedEventArgs e) => Make(null);

    /// <summary>Creates a manual backup zip of the selected profile (or the currently active one) in launcher\data\save_backups.
    /// Backups are also made automatically before updates and profile switches.</summary>
    void Backup_Click(object sender, RoutedEventArgs e)
    {
        var p = List.SelectedItem as Profile ?? Profiles.Active(MainWindow.Settings);
        try
        {
            var zip = SaveBackup.Make(p, "manual");
            MessageBox.Show(zip == null ? $"“{p.DisplayName}” has no saves yet." : $"Saved {Path.GetFileName(zip)}.\n\nBackups are also made before updates and before switching profiles; the newest 10 of each profile are kept.", "Back up saves");
        }
        catch (Exception ex) { MessageBox.Show(ex.Message, "Couldn't back up the saves"); }
    }
    void OpenBackups_Click(object sender, RoutedEventArgs e) { Directory.CreateDirectory(SaveBackup.Dir); System.Diagnostics.Process.Start("explorer.exe", SaveBackup.Dir); }
    void Duplicate_Click(object sender, RoutedEventArgs e) { if (List.SelectedItem is Profile p) Make(p); }

    /// <summary>Creates a new save profile (from scratch or duplicating an existing one).
    /// Prompts for a name, creates the profile, and optionally activates it.</summary>
    void Make(Profile? from)
    {
        var name = Prompt.Ask(from == null ? "New profile" : "Duplicate profile", "Name of the new profile:",
                              from == null ? "Fresh start" : (from.IsDefault ? "Default" : from.Name) + " copy");
        if (string.IsNullOrWhiteSpace(name)) return;
        try
        {
            var dir = Profiles.Create(name, from);
            if (MessageBox.Show($"Use “{Path.GetFileName(dir)}” now?", "Save profiles", MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes && !GameRunningWarning())
            { MainWindow.Settings.ActiveProfile = Path.GetFileName(dir); MainWindow.Settings.Save(); }
        }
        catch (Exception ex) { MessageBox.Show(ex.Message, "Couldn't create the profile", MessageBoxButton.OK, MessageBoxImage.Error); }
        Refresh();
    }

    /// <summary>Renames the selected profile, skipping the default profile.</summary>
    void Rename_Click(object sender, RoutedEventArgs e)
    {
        if (List.SelectedItem is not Profile p || p.IsDefault || GameRunningWarning()) return;
        var name = Prompt.Ask("Rename profile", "New name:", p.Name);
        if (string.IsNullOrWhiteSpace(name) || name == p.Name) return;
        try { Profiles.Rename(p, name, MainWindow.Settings); MainWindow.Settings.Save(); }
        catch (Exception ex) { MessageBox.Show(ex.Message, "Rename failed", MessageBoxButton.OK, MessageBoxImage.Error); }
        Refresh();
    }

    /// <summary>Deletes the selected profile and its saves, skipping the default profile.</summary>
    void Delete_Click(object sender, RoutedEventArgs e)
    {
        if (List.SelectedItem is not Profile p || p.IsDefault || GameRunningWarning()) return;
        if (MessageBox.Show($"Delete the profile “{p.Name}” and its saves? This can't be undone.\n{p.Folder}", "Delete profile",
                MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes) return;
        try { Profiles.Delete(p, MainWindow.Settings); MainWindow.Settings.Save(); }
        catch (Exception ex) { MessageBox.Show(ex.Message, "Delete failed", MessageBoxButton.OK, MessageBoxImage.Error); }
        Refresh();
    }

    /// <summary>Opens the selected profile's folder in Windows Explorer.</summary>
    void Open_Click(object sender, RoutedEventArgs e)
    {
        if (List.SelectedItem is not Profile p) return;
        Directory.CreateDirectory(p.Folder);
        Process.Start("explorer.exe", p.Folder);
    }
}
