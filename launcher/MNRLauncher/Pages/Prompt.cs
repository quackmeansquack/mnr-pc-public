using System.Windows;
using System.Windows.Controls;

namespace MNRLauncher.Pages;

/// <summary>A small text-input dialog in the launcher's style.</summary>
public static class Prompt
{
    public static string? Ask(string title, string label, string initial = "", bool multiline = false)
    {
        var box = new TextBox { Text = initial, Margin = new Thickness(0, 8, 0, 14), AcceptsReturn = multiline, TextWrapping = multiline ? TextWrapping.Wrap : TextWrapping.NoWrap, Height = multiline ? 90 : double.NaN };
        var ok = new Button { Content = "OK", IsDefault = true, Style = (Style)Application.Current.Resources["Primary"], MinWidth = 80 };
        var cancel = new Button { Content = "Cancel", IsCancel = true, MinWidth = 80, Margin = new Thickness(0) };
        var win = new Window
        {
            Title = title, Width = 420, SizeToContent = SizeToContent.Height, ResizeMode = ResizeMode.NoResize, WindowStartupLocation = WindowStartupLocation.CenterOwner,
            Owner = Application.Current.MainWindow, Background = (System.Windows.Media.Brush)Application.Current.Resources["Bg"],
            Content = new StackPanel
            {
                Margin = new Thickness(18),
                Children = { new TextBlock { Text = label }, box, new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Children = { ok, cancel } } }
            }
        };
        ok.Click += (_, _) => win.DialogResult = true;
        win.Loaded += (_, _) => { box.Focus(); box.SelectAll(); };
        return win.ShowDialog() == true ? box.Text.Trim() : null;
    }
}
