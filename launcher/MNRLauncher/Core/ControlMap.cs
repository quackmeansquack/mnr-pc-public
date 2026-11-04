using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;

namespace MNRLauncher.Core;

/// <summary>One PS3 control bound to a controller's axis, button or hat direction.</summary>
public class Binding
{
    public string Control { get; set; } = "";
    public string Device { get; set; } = "";      // "046D:C262" (USB vendor:product)
    public string Kind { get; set; } = "axis";    // axis, button, pov
    public int Index { get; set; }
    public bool Invert { get; set; }
    public double Deadzone { get; set; }
    public double Low { get; set; }
    public double High { get; set; } = 65535;

    /// <summary>Describes this binding in human-readable form, resolving the device name from the connected devices list.</summary>
    public string Describe(IEnumerable<Joystick.Device> devices)
    {
        var dev = devices.FirstOrDefault(d => d.Key.Equals(Device, StringComparison.OrdinalIgnoreCase));
        var name = dev?.Name ?? Device + " (not connected)";
        var src = Kind switch
        {
            "axis" => $"axis {Joystick.AxisNames[Math.Clamp(Index, 0, 5)]}" + (Low > High ? " (reversed)" : "") + (Invert ? " inverted" : ""),
            "button" => $"button {Index + 1}",
            _ => "hat " + new[] { "up", "right", "down", "left" }[Math.Clamp(Index, 0, 3)],
        };
        return $"{src}  ·  {name}";
    }
}

/// <summary>The PS3 controls a device can drive, with their names in the launcher.</summary>
public record ControlDef(string Id, string Name, bool Axis, string Group);

/// <summary>Bindings for wheels, pedals and other controllers; saved to MNR\launcher\data\controls.cfg, which the game
/// reads through MNR_INPUT_CONFIG (format in re\recomp\input.c).</summary>
public static class ControlMap
{
    public static string File => Path.Combine(Paths.LauncherData, "controls.cfg");

    /// <summary>The complete list of PS3 controls the game uses, with launcher display names and grouping.</summary>
    public static readonly ControlDef[] Controls =
    {
        new("lx", "Steering (left stick left/right)", true, "Driving"),
        new("r2", "Accelerate (R2)", true, "Driving"),
        new("l2", "L2", true, "Driving"),
        new("r1", "R1", false, "Driving"),
        new("ly", "Left stick up/down", true, "Driving"),
        new("square", "Square", false, "Buttons"),
        new("cross", "Cross", false, "Buttons"),
        new("circle", "Circle", false, "Buttons"),
        new("triangle", "Triangle", false, "Buttons"),
        new("l1", "L1", false, "Buttons"),
        new("start", "Start", false, "Buttons"),
        new("select", "Select", false, "Buttons"),
        new("up", "D-pad up", false, "Buttons"),
        new("down", "D-pad down", false, "Buttons"),
        new("left", "D-pad left", false, "Buttons"),
        new("right", "D-pad right", false, "Buttons"),
        new("l3", "L3 (left stick click)", false, "Buttons"),
        new("r3", "R3 (right stick click: players and friends list)", false, "Buttons"),
        new("rx", "Right stick left/right", true, "Buttons"),
        new("ry", "Right stick up/down", true, "Buttons"),
    };

    /// <summary>
    /// Wheel configuration options passed to the game's input system: rotation degrees, steering lock/boost,
    /// force feedback on/off, centring spring, and damper. All values match re\recomp\input.c.
    /// </summary>
    public class WheelOptions
    {
        public double WheelDegrees { get; set; } = 900;   // the wheel's rotation, as set in its driver (G HUB: 900 for a G920)
        public double SteerLock { get; set; } = 270;      // rotation that gives full lock in the game
        public double SteerBoost { get; set; } = 15;      // % of steering as soon as the wheel leaves the centre
        public bool ForceFeedback { get; set; }
        public double Spring { get; set; } = 35;          // centring spring 0-100
        public double Damper { get; set; } = 20;          // damper 0-100
    }

    /// <summary>
    /// Parses controls.cfg for wheel option values (wheel_degrees, steer_lock, steer_boost, ffb, ffb_spring, ffb_damper).
    /// Returns a new WheelOptions with defaults if the file doesn't exist.
    /// </summary>
    public static WheelOptions LoadOptions()
    {
        var o = new WheelOptions();
        if (!System.IO.File.Exists(File)) return o;
        foreach (var raw in System.IO.File.ReadAllLines(File))
        {
            var f = raw.Split('#')[0].Split('|').Select(x => x.Trim()).ToArray();
            if (f.Length < 3 || f[0] != "option" || !double.TryParse(f[2], NumberStyles.Float, CultureInfo.InvariantCulture, out var v)) continue;
            switch (f[1])
            {
                case "wheel_degrees": o.WheelDegrees = v; break;
                case "steer_lock": o.SteerLock = v > 0 ? v : 270; break;   // 0 (the whole wheel) was far too slow on a 900° wheel
                case "steer_boost": o.SteerBoost = v; break;
                case "ffb": o.ForceFeedback = v > 0; break;
                case "ffb_spring": o.Spring = v; break;
                case "ffb_damper": o.Damper = v; break;
            }
        }
        return o;
    }

    /// <summary>
    /// Parses controls.cfg for binding lines (control | device | kind | index | invert | deadzone | low | high).
    /// Skips "option" lines and comments. Returns the list of parsed bindings.
    /// </summary>
    public static List<Binding> Load()
    {
        var list = new List<Binding>();
        if (!System.IO.File.Exists(File)) return list;
        foreach (var raw in System.IO.File.ReadAllLines(File))
        {
            var line = raw.Split('#')[0];
            var f = line.Split('|').Select(x => x.Trim()).ToArray();
            if (f.Length < 4 || f[0] == "option") continue;
            double D(int i, double def) => f.Length > i && double.TryParse(f[i], NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : def;
            list.Add(new Binding
            {
                Control = f[0], Device = f[1], Kind = f[2], Index = int.TryParse(f[3], out var ix) ? ix : 0,
                Invert = f.Length > 4 && f[4] == "1", Deadzone = D(5, 0), Low = D(6, 0), High = D(7, 65535),
            });
        }
        return list;
    }

    public static void Save(List<Binding> list) => Save(list, LoadOptions());

    /// <summary>
    /// Writes all bindings and wheel options to controls.cfg. Format matches re\recomp\input.c.
    /// </summary>
    public static void Save(List<Binding> list, WheelOptions o)
    {
        Directory.CreateDirectory(Paths.LauncherData);
        var lines = new List<string> { "# control | device | kind | index | invert | deadzone | low | high   (written by the launcher; see re\\recomp\\input.c)" };
        lines.AddRange(list.Select(b => string.Join(" | ", b.Control, b.Device, b.Kind, b.Index, b.Invert ? 1 : 0,
            b.Deadzone.ToString("0.###", CultureInfo.InvariantCulture), b.Low.ToString("0", CultureInfo.InvariantCulture), b.High.ToString("0", CultureInfo.InvariantCulture))));
        string N(double v) => v.ToString("0.##", CultureInfo.InvariantCulture);
        lines.Add($"option | wheel_degrees | {N(o.WheelDegrees)}");
        lines.Add($"option | steer_lock | {N(o.SteerLock)}");
        lines.Add($"option | steer_boost | {N(o.SteerBoost)}");
        lines.Add($"option | ffb | {(o.ForceFeedback ? 1 : 0)}");
        lines.Add($"option | ffb_spring | {N(o.Spring)}");
        lines.Add($"option | ffb_damper | {N(o.Damper)}");
        System.IO.File.WriteAllLines(File, lines);
    }
}
