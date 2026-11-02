using System;
using System.Linq;
using System.Windows;
using System.Windows.Controls;

namespace MNRLauncher.Pages;

/// <summary>A sidebar entry holding several pages as tabs (Creations: browse + installed; Profiles: accounts + saves;
/// Settings: its sections). Each page is made when its tab is first opened and refreshed whenever it is shown.</summary>
public class TabsPage : UserControl, IRefresh
{
    readonly TabControl tabs = new() { Margin = new Thickness(24, 10, 24, 0), BorderThickness = new Thickness(0) };
    readonly Func<UserControl>[] makers;
    readonly UserControl?[] made;

    public TabsPage(params (string name, Func<UserControl> make)[] pages)
    {
        makers = pages.Select(p => p.make).ToArray();
        made = new UserControl?[pages.Length];
        foreach (var (name, _) in pages) tabs.Items.Add(new TabItem { Header = name });
        tabs.SelectionChanged += (s, e) => { if (e.Source == tabs) Show(); };
        Content = tabs;
    }

    public int Count => makers.Length;
    public UserControl? Current => tabs.SelectedIndex >= 0 ? made[tabs.SelectedIndex] : null;

    public void Select(int index) { tabs.SelectedIndex = index; Show(); }

    void Show()
    {
        int i = tabs.SelectedIndex;
        if (i < 0) return;
        var tab = (TabItem)tabs.Items[i];
        if (made[i] == null) tab.Content = made[i] = makers[i]();
        if (made[i] is IRefresh r) r.Refresh();
    }

    public void Refresh() { if (tabs.SelectedIndex < 0) tabs.SelectedIndex = 0; else Show(); }
}
