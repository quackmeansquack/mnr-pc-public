using System;
using System.Linq;
using System.Windows;
using System.Windows.Media;
using Wpf.Ui.Appearance;

namespace MNRLauncher.Core;

/// <summary>The launcher's colour themes, taken from the game's own screens. WPF UI (Fluent) draws the controls in its
/// light or dark style with the theme's accent; the launcher's own brushes (Bg, Panel, Text, Accent, ...) are swapped in
/// Application.Resources, and the pages refer to them as DynamicResource, so a new theme shows at once.</summary>
public static class Themes
{
    public record Theme(string Name, string From, bool Light, string Bg, string Panel, string PanelHi, string Line,
                        string Text, string SubText, string Accent, string OnAccent, string Accent2);

    public static readonly Theme[] All =
    {
        new("Pit lane", "the game's orange dialog boxes over charcoal", false,
            "#FF1F1F21", "#FF19191B", "#FF2A2A2D", "#FF3A3A3E", "#FFF3F1EC", "#FFA9A6A0", "#FFF59A00", "#FF2B1800", "#FFFFC94A"),
        new("Checkered red", "the HUD's red position numbers and flag stripes", false,
            "#FF1B1A1E", "#FF151418", "#FF26252A", "#FF38363D", "#FFF5F4F7", "#FFA3A0AA", "#FFE0352B", "#FFFFFFFF", "#FFFFFFFF"),
        new("Boost", "the green flag, boost meter and pink item icon", false,
            "#FF181C18", "#FF131613", "#FF232823", "#FF343B34", "#FFEEF3EC", "#FF9CA79A", "#FF7CC243", "#FF142406", "#FFE8358F"),
        new("Kart park night", "the Kart Park after dark: amber lights on navy", false,
            "#FF141C30", "#FF101727", "#FF1D2840", "#FF2C3A57", "#FFEEF2FA", "#FF9AA6BF", "#FFFFB300", "#FF2A1C00", "#FF5BC0F0"),
        new("Studio light", "the Creation Station's warm paper and orange", true,
            "#FFF5F0E7", "#FFECE5D8", "#FFFFFFFF", "#FFDDD3C2", "#FF2B2118", "#FF7A6C5C", "#FFE87500", "#FFFFFFFF", "#FF2B2118"),
    };
    public const string Default = "Pit lane";

    public static Theme Get(string? name) => All.FirstOrDefault(t => t.Name == name) ?? All[0];

    static Color C(string hex) => (Color)ColorConverter.ConvertFromString(hex);
    static Color Mix(Color a, Color b, double t) =>
        Color.FromArgb(255, (byte)(a.R + (b.R - a.R) * t), (byte)(a.G + (b.G - a.G) * t), (byte)(a.B + (b.B - a.B) * t));

    public static void Apply(string? name)
    {
        var t = Get(name);
        var r = Application.Current.Resources;
        var mode = t.Light ? ApplicationTheme.Light : ApplicationTheme.Dark;
        ApplicationThemeManager.Apply(mode, Wpf.Ui.Controls.WindowBackdropType.None, false);
        ApplicationAccentColorManager.Apply(C(t.Accent), mode, false);

        void Brush(string key, Color c) { var b = new SolidColorBrush(c); b.Freeze(); r[key] = b; }
        Brush("Bg", C(t.Bg)); r["BgColor"] = C(t.Bg);
        Brush("Panel", C(t.Panel)); Brush("PanelHi", C(t.PanelHi)); Brush("Line", C(t.Line));
        Brush("Text", C(t.Text)); Brush("SubText", C(t.SubText));
        Brush("Accent", C(t.Accent)); Brush("AccentHi", Mix(C(t.Accent), t.Light ? Colors.Black : Colors.White, 0.18));
        Brush("OnAccent", C(t.OnAccent)); Brush("Accent2", C(t.Accent2));
        Brush("Good", t.Light ? C("#FF2E8B57") : C("#FF4CC38A")); Brush("Warn", t.Light ? C("#FFB7791F") : C("#FFF0B429"));
        // WPF UI's own keys: the window background and the text on accent-coloured buttons follow the theme
        r["ApplicationBackgroundColor"] = C(t.Bg); Brush("ApplicationBackgroundBrush", C(t.Bg));
        r["TextOnAccentFillColorPrimary"] = C(t.OnAccent); Brush("TextOnAccentFillColorPrimaryBrush", C(t.OnAccent));
    }
}
