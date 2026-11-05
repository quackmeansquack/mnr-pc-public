using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;
using MNRLauncher.Core;

namespace MNRLauncher.Pages;

/// <summary>Mods page: imports, orders, enables and deletes mods.</summary>
public partial class ModsPage : UserControl, IRefresh
{
    List<Mod> mods = new();
    /// <summary>Creates the mods page.</summary>
    public ModsPage() { InitializeComponent(); }

    /// <summary>Rescans the mods folder and repopulates the list, keeping the previous selection.</summary>
    public void Refresh()
    {
        var keep = (List.SelectedItem as Mod)?.Id;
        mods = ModManager.Scan(MainWindow.Settings);
        List.ItemsSource = null; List.ItemsSource = mods;
        List.SelectedItem = mods.FirstOrDefault(m => m.Id == keep);
        Empty.Visibility = mods.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    /// <summary>Saves the enabled mods list to settings, preserving the order shown in the UI.</summary>
    void SaveOrder()
    {
        MainWindow.Settings.EnabledMods = mods.Where(m => m.Enabled).Select(m => m.Id).ToList();
        MainWindow.Settings.Save();
    }

    /// <summary>Toggles a mod's enabled state and reorders it to the end of the enabled group.</summary>
    void Toggle(object sender, RoutedEventArgs e)
    {
        if ((sender as CheckBox)?.DataContext is not Mod m) return;
        bool on = ((CheckBox)sender).IsChecked == true;
        if (m.Enabled == on) return;   // (re-binding the list sets the boxes too)
        m.Enabled = on;
        // newly enabled mods go to the end of the enabled group
        mods = mods.Where(x => x.Enabled && x != m).Concat(m.Enabled ? new[] { m } : Array.Empty<Mod>()).Concat(mods.Where(x => !x.Enabled && x != m)).Concat(m.Enabled ? Array.Empty<Mod>() : new[] { m }).ToList();
        SaveOrder(); Refresh();
    }

    /// <summary>Moves a mod up or down in the enabled group (d = -1 for up, d = 1 for down).</summary>
    void Move(int d)
    {
        if (List.SelectedItem is not Mod m) return;
        int i = mods.IndexOf(m), j = i + d;
        if (j < 0 || j >= mods.Count || mods[j].Enabled != m.Enabled) return;   // reorder within the enabled group
        (mods[i], mods[j]) = (mods[j], mods[i]);
        SaveOrder(); List.ItemsSource = null; List.ItemsSource = mods; List.SelectedItem = m;
    }
    void Up_Click(object sender, RoutedEventArgs e) => Move(-1);
    void Down_Click(object sender, RoutedEventArgs e) => Move(1);

    /// <summary>Imports mod files from .mnrmod, .zip, or .pkg files. For .pkg files, extracts the USRDIR as the mod's files.</summary>
    void Import_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog { Filter = "ModNation mods (*.mnrmod;*.zip;*.pkg)|*.mnrmod;*.zip;*.pkg", Multiselect = true };
        if (dlg.ShowDialog() != true) return;
        foreach (var f in dlg.FileNames)
        {
            try
            {
                if (!f.EndsWith(".pkg", StringComparison.OrdinalIgnoreCase)) { ModManager.Import(f); continue; }
                // a PS3 package (debug or retail) of game files: its USRDIR becomes the mod's files
                var r = PkgInstaller.ExtractMod(f, Paths.ModsDir);
                if (r.Skipped.Count > 0)
                    MessageBox.Show($"Imported “{r.Name}” ({r.Files} files). Left out:\n" + string.Join("\n", r.Skipped.Take(15)) + (r.Skipped.Count > 15 ? $"\n... and {r.Skipped.Count - 15} more" : ""),
                        "Imported package", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            catch (Exception ex) { MessageBox.Show($"{Path.GetFileName(f)}: {ex.Message}", "Import failed", MessageBoxButton.OK, MessageBoxImage.Error); }
        }
        Refresh();
    }

    /// <summary>Exports the selected mod as an .mnrmod file to a user-selected location.</summary>
    void Export_Click(object sender, RoutedEventArgs e)
    {
        if (List.SelectedItem is not Mod m) return;
        var dlg = new SaveFileDialog { Filter = "ModNation mod (*.mnrmod)|*.mnrmod", FileName = ModManager.SafeName(m.DisplayName) + ".mnrmod" };
        if (dlg.ShowDialog() != true) return;
        try { ModManager.Export(m, dlg.FileName); MessageBox.Show($"Saved {Path.GetFileName(dlg.FileName)}.", "Export", MessageBoxButton.OK, MessageBoxImage.Information); }
        catch (Exception ex) { MessageBox.Show(ex.Message, "Export failed", MessageBoxButton.OK, MessageBoxImage.Error); }
    }

    /// <summary>Creates a new empty mod with the given name and opens its folder in Explorer.</summary>
    void New_Click(object sender, RoutedEventArgs e)
    {
        var name = Prompt.Ask("New mod", "Name of the mod:", "My mod");
        if (string.IsNullOrWhiteSpace(name)) return;
        var dir = ModManager.CreateEmpty(name, Accounts.ExportName(MainWindow.Settings));
        Refresh();
        Process.Start("explorer.exe", dir);
    }

    /// <summary>Opens the selected mod's folder in Windows Explorer.</summary>
    void Show_Click(object sender, RoutedEventArgs e) { if (List.SelectedItem is Mod m) Process.Start("explorer.exe", m.Folder); }
    /// <summary>Opens the global mods directory in Windows Explorer.</summary>
    void OpenFolder_Click(object sender, RoutedEventArgs e) { Directory.CreateDirectory(Paths.ModsDir); Process.Start("explorer.exe", Paths.ModsDir); }

    /// <summary>Deletes the selected mod and its folder from disk.</summary>
    void Delete_Click(object sender, RoutedEventArgs e)
    {
        if (List.SelectedItem is not Mod m) return;
        if (MessageBox.Show($"Delete the mod “{m.DisplayName}” and its folder?\n{m.Folder}", "Delete mod", MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes) return;
        try { Directory.Delete(m.Folder, true); } catch (Exception ex) { MessageBox.Show(ex.Message, "Delete failed"); }
        MainWindow.Settings.EnabledMods.Remove(m.Id); MainWindow.Settings.Save();
        Refresh();
    }
}
