using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using MNRLauncher.Core;

namespace MNRLauncher.Pages;

/// <summary>Settings &gt; Controls tab: maps keyboard, controller and wheel inputs and configures the wheel.</summary>
public partial class ControlsPage : UserControl, IRefresh
{
    /// <summary>A UI row displaying a control definition and its current bindings.</summary>
    public class Row
    {
        public ControlDef Def { get; init; } = null!;
        public string Text { get; set; } = "";
    }

    List<Joystick.Device> devices = new();
    List<Binding> binds = new();
    readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromMilliseconds(33) };
    readonly ProgressBar[] axisBars = new ProgressBar[6];
    readonly TextBlock buttonsText = new() { Style = null, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 6, 0, 0) };

    // listening for a binding
    Row? listening;
    DateTime listenStart;
    readonly Dictionary<uint, Joystick.JOYINFOEX> baseline = new();
    (uint dev, int axis, uint rest, uint extreme)? moving;

    /// <summary>Creates the controls page.</summary>
    public ControlsPage()
    {
        InitializeComponent();
        for (int i = 0; i < 6; i++)
        {
            var row = new DockPanel { Margin = new Thickness(0, 2, 0, 2) };
            var label = new TextBlock { Text = "Axis " + Joystick.AxisNames[i], Width = 52, Foreground = (Brush)FindResource("SubText") };
            DockPanel.SetDock(label, Dock.Left);
            axisBars[i] = new ProgressBar { Maximum = 65535, Height = 10, Background = (Brush)FindResource("Panel"), Foreground = (Brush)FindResource("Accent"), BorderThickness = new Thickness(0) };
            row.Children.Add(label); row.Children.Add(axisBars[i]);
            Live.Children.Add(row);
        }
        buttonsText.Foreground = (Brush)FindResource("SubText");
        Live.Children.Add(buttonsText);
        timer.Tick += Tick;
        Loaded += (_, _) => timer.Start();
        Unloaded += (_, _) => { timer.Stop(); StopListening(null); };
        PreviewKeyDown += (_, e) => { if (e.Key == Key.Escape && listening != null) StopListening("Cancelled."); };
    }

    /// <summary>Refreshes the controls page: lists available joysticks, loads control bindings, and displays wheel options.</summary>
    public void Refresh()
    {
        devices = Joystick.List();
        Devices.ItemsSource = devices;
        if (devices.Count > 0 && Devices.SelectedIndex < 0) Devices.SelectedIndex = 0;
        binds = ControlMap.Load();
        ShowRows();
        var o = ControlMap.LoadOptions();
        WheelDeg.Text = o.WheelDegrees.ToString("0"); SteerLock.Text = o.SteerLock.ToString("0");
        Ffb.IsChecked = o.ForceFeedback; Boost.Value = o.SteerBoost; Spring.Value = o.Spring; Damper.Value = o.Damper; WheelSaved.Text = "";
    }

    /// <summary>Populates the UI rows with control definitions and their current bindings.</summary>
    void ShowRows()
    {
        Rows.ItemsSource = ControlMap.Controls.Select(d =>
        {
            var b = binds.Where(x => x.Control == d.Id || x.Control.StartsWith(d.Id + "-") || x.Control.StartsWith(d.Id + "+")).ToList();
            return new Row { Def = d, Text = b.Count == 0 ? "not bound" : string.Join("   +   ", b.Select(x => x.Describe(devices))) };
        }).ToList();
    }

    /// <summary>Rescans for available joysticks and reloads the controls page.</summary>
    void Rescan_Click(object sender, RoutedEventArgs e) => Refresh();

    void Ffb_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (SpringText == null || DamperText == null || BoostText == null) return;
        SpringText.Text = $"{Spring.Value:0}%"; DamperText.Text = $"{Damper.Value:0}%"; BoostText.Text = $"{Boost.Value:0}%";
    }

    /// <summary>Applies wheel configuration settings (degrees, steer lock, force feedback, steering boost, spring/damper).</summary>
    void ApplyWheel_Click(object sender, RoutedEventArgs e)
    {
        if (!double.TryParse(WheelDeg.Text, out var deg) || deg < 0 || deg > 3600 || !double.TryParse(SteerLock.Text, out var lk) || lk < 0 || lk > 3600)
        { WheelSaved.Text = "  Enter degrees as numbers (0 to 3600)."; return; }
        ControlMap.Save(binds, new ControlMap.WheelOptions { WheelDegrees = deg, SteerLock = lk, ForceFeedback = Ffb.IsChecked == true, SteerBoost = Math.Round(Boost.Value), Spring = Math.Round(Spring.Value), Damper = Math.Round(Damper.Value) });
        WheelSaved.Text = "  Saved: used the next time the game starts.";
    }
    void Devices_SelectionChanged(object sender, SelectionChangedEventArgs e) { }

    /// <summary>Periodic tick handler: reads joystick state, updates axis bars and button display, and processes listening for bindings.</summary>
    void Tick(object? sender, EventArgs e)
    {
        if (Devices.SelectedItem is Joystick.Device d && Joystick.Read(d.Id, out var st))
        {
            for (int i = 0; i < 6; i++) { axisBars[i].Value = Joystick.Axis(st, i); axisBars[i].Visibility = i < Math.Max(d.Axes, 2) ? Visibility.Visible : Visibility.Collapsed; }
            var pressed = Enumerable.Range(0, 32).Where(i => (st.dwButtons >> i & 1) != 0).Select(i => (i + 1).ToString()).ToList();
            buttonsText.Text = "Buttons held: " + (pressed.Count == 0 ? "none" : string.Join(", ", pressed)) + (st.dwPOV != 0xFFFF ? $"   ·   hat {st.dwPOV / 100}°" : "");
        }
        if (listening != null) Listen();
    }

    /// <summary>Initiates binding capture for a control: prompts the user to press a button, move an axis, or press a pedal.</summary>
    void Bind_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as Button)?.Tag is not Row r) return;
        devices = Joystick.List();
        if (devices.Count == 0) { MessageBox.Show("No game controllers found. Connect the wheel or pedals and click Refresh list.", "Controls"); return; }
        baseline.Clear();
        foreach (var d in devices) if (Joystick.Read(d.Id, out var st)) baseline[d.Id] = st;
        listening = r; moving = null; listenStart = DateTime.Now;
        ListeningText.Text = r.Def.Axis && r.Def.Id is "lx" or "rx" ? $"{r.Def.Name}: turn / push it to the RIGHT and back…   (Esc cancels)"
                           : r.Def.Axis && r.Def.Id is "ly" or "ry" ? $"{r.Def.Name}: push it DOWN and back…   (Esc cancels)"
                           : $"{r.Def.Name}: press the button or pedal (all the way) and release…   (Esc cancels)";
        Listening.Visibility = Visibility.Visible;
        Focus();
    }

    /// <summary>Continuously polls joystick devices to detect button presses, POV hat directions, and axis movements for binding capture.</summary>
    void Listen()
    {
        var r = listening!;
        if ((DateTime.Now - listenStart).TotalSeconds > 12) { StopListening("Nothing moved; try again."); return; }
        foreach (var d in devices)
        {
            if (!baseline.TryGetValue(d.Id, out var b) || !Joystick.Read(d.Id, out var st)) continue;
            // a button that wasn't held when we started
            uint newly = st.dwButtons & ~b.dwButtons;
            if (newly != 0 && moving == null)
            {
                int bit = System.Numerics.BitOperations.TrailingZeroCount(newly);
                Add(new Binding { Control = r.Def.Id, Device = d.Key, Kind = "button", Index = bit });
                return;
            }
            if (st.dwPOV != 0xFFFF && b.dwPOV == 0xFFFF && moving == null)
            {
                int dir = (int)Math.Round(st.dwPOV / 9000.0) % 4;
                Add(new Binding { Control = r.Def.Id, Device = d.Key, Kind = "pov", Index = dir });
                return;
            }
            // an axis: follow it while it moves, bind when it comes back (or after 3 s)
            for (int i = 0; i < Math.Max(d.Axes, 2) && i < 6; i++)
            {
                uint rest = Joystick.Axis(b, i), now = Joystick.Axis(st, i);
                if (moving is { } m && m.dev == d.Id && m.axis == i)
                {
                    uint ext = Math.Abs((long)now - m.rest) > Math.Abs((long)m.extreme - m.rest) ? now : m.extreme;
                    moving = (d.Id, i, m.rest, ext);
                    if (Math.Abs((long)now - m.rest) < 6000 || (DateTime.Now - listenStart).TotalSeconds > 6) { FinishAxis(d, i, m.rest, ext); return; }
                }
                else if (moving == null && Math.Abs((long)now - rest) > 20000) moving = (d.Id, i, rest, now);
            }
        }
    }

    /// <summary>Finalizes an axis binding with appropriate low/high values, deadzone, and inversion based on the detected movement direction.</summary>
    void FinishAxis(Joystick.Device d, int axis, uint rest, uint extreme)
    {
        var r = listening!;
        bool full = r.Def.Axis && r.Def.Id is "lx" or "ly" or "rx" or "ry";
        var b = new Binding { Control = r.Def.Id, Device = d.Key, Kind = "axis", Index = axis };
        if (full) { b.Low = 0; b.High = 65535; b.Invert = extreme < rest; b.Deadzone = 0.04; }   // moved right/down: should rise
        else { b.Low = rest; b.High = extreme; b.Deadzone = 0.05; }                            // pedal: rest -> fully pressed
        Add(b);
    }

    /// <summary>Adds a binding to the list, saves it, and refreshes the UI rows.</summary>
    void Add(Binding b)
    {
        binds.RemoveAll(x => x.Control == b.Control);
        binds.Add(b);
        ControlMap.Save(binds);
        StopListening(null);
        ShowRows();
    }

    /// <summary>Stops the current binding capture and optionally displays a status message.</summary>
    void StopListening(string? message)
    {
        listening = null; moving = null;
        if (message == null) { Listening.Visibility = Visibility.Collapsed; return; }
        ListeningText.Text = message;
        var t = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
        t.Tick += (_, _) => { t.Stop(); if (listening == null) Listening.Visibility = Visibility.Collapsed; };
        t.Start();
    }

    /// <summary>Clears all bindings for the selected control definition.</summary>
    void Clear_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as Button)?.Tag is not Row r) return;
        binds.RemoveAll(x => x.Control == r.Def.Id || x.Control.StartsWith(r.Def.Id + "-") || x.Control.StartsWith(r.Def.Id + "+"));
        ControlMap.Save(binds); ShowRows();
    }

    /// <summary>Clears all control bindings after user confirmation.</summary>
    void ClearAll_Click(object sender, RoutedEventArgs e)
    {
        if (binds.Count == 0 || MessageBox.Show("Remove all bindings?", "Controls", MessageBoxButton.YesNo) != MessageBoxResult.Yes) return;
        binds.Clear(); ControlMap.Save(binds); ShowRows();
    }
}
