using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;
using MNRLauncher.Core;

namespace MNRLauncher.Pages;

public partial class CreationsPage : UserControl, IRefresh
{
    public CreationsPage() { InitializeComponent(); }

    /// <summary>Scans the creations directory and populates the UI list with available creations.</summary>
    public void Refresh()
    {
        var list = CreationStore.Scan();
        List.ItemsSource = list;
        Empty.Visibility = list.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    /// <summary>Exports the selected creation to an .mnrcreation file via a save dialog.</summary>
    void Export_Click(object sender, RoutedEventArgs e)
    {
        if (List.SelectedItem is not Creation c) return;
        var dlg = new SaveFileDialog { Filter = "ModNation creation (*.mnrcreation)|*.mnrcreation", FileName = ModManager.SafeName(c.Name) + ".mnrcreation" };
        if (dlg.ShowDialog() != true) return;
        var desc = Prompt.Ask("Export", "Description (optional):", "", multiline: true) ?? "";
        try
        {
            CreationStore.Export(c, dlg.FileName, Accounts.ExportName(MainWindow.Settings), desc);
            MessageBox.Show($"Saved {Path.GetFileName(dlg.FileName)}. Anyone can import it in their launcher.", "Export", MessageBoxButton.OK, MessageBoxImage.Information);
        }
        catch (Exception ex) { MessageBox.Show(ex.Message, "Export failed", MessageBoxButton.OK, MessageBoxImage.Error); }
    }

    /// <summary>Imports .mnrcreation files and resolves file collisions with existing creations.</summary>
    void Import_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog { Filter = "ModNation creations (*.mnrcreation)|*.mnrcreation", Multiselect = true };
        if (dlg.ShowDialog() != true) return;
        foreach (var f in dlg.FileNames)
        {
            try
            {
                var clash = CreationStore.Collisions(f);
                if (clash.Count > 0 && MessageBox.Show($"{Path.GetFileName(f)} would replace {clash.Count} existing file(s):\n{string.Join("\n", clash.Take(6))}\n\nReplace them?",
                        "Import", MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes) continue;
                var m = CreationStore.Import(f);
            }
            catch (Exception ex) { MessageBox.Show($"{Path.GetFileName(f)}: {ex.Message}", "Import failed", MessageBoxButton.OK, MessageBoxImage.Error); }
        }
        Refresh();
    }

    /// <summary>Opens the selected creation's folder in Windows Explorer, selecting the first file.</summary>
    void Show_Click(object sender, RoutedEventArgs e)
    {
        if (List.SelectedItem is Creation c && c.Files.Count > 0) Process.Start("explorer.exe", "/select,\"" + Path.Combine(Paths.CreationsDir, c.Files[0]) + "\"");
    }
    /// <summary>Checks all creations for installation issues and displays the results in a message box.</summary>
    void Check_Click(object sender, RoutedEventArgs e)
    {
        var notes = CreationInstaller.Check();
        MessageBox.Show(string.Join("\n\n", notes.Take(20)) + (notes.Count > 20 ? $"\n\n…and {notes.Count - 20} more." : ""), "Check my creations");
        Refresh();
    }

    /// <summary>Opens the global creations directory in Windows Explorer.</summary>
    void OpenFolder_Click(object sender, RoutedEventArgs e) { Directory.CreateDirectory(Paths.CreationsDir); Process.Start("explorer.exe", Paths.CreationsDir); }
}
