using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace MNRLauncher.Core;

/// <summary>Game controllers as the Windows joystick API (winmm) sees them, which is also how the game reads them
/// (re\recomp\input.c): up to 16 devices, 6 axes (X Y Z R U V, 0-65535), 32 buttons and a hat.</summary>
public static class Joystick
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    /// <summary>WinMM joystick capabilities structure (joyGetDevCaps).</summary>
    struct JOYCAPSW
    {
        public ushort wMid, wPid;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szPname;
        public uint wXmin, wXmax, wYmin, wYmax, wZmin, wZmax, wNumButtons, wPeriodMin, wPeriodMax, wRmin, wRmax, wUmin, wUmax, wVmin, wVmax, wCaps, wMaxAxes, wNumAxes, wMaxButtons;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szRegKey;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string szOEMVxD;
    }
    [StructLayout(LayoutKind.Sequential)]
    /// <summary>WinMM joystick state structure (joyGetPosEx).</summary>
    public struct JOYINFOEX
    {
        public uint dwSize, dwFlags, dwXpos, dwYpos, dwZpos, dwRpos, dwUpos, dwVpos, dwButtons, dwButtonNumber, dwPOV, dwReserved1, dwReserved2;
    }
    [DllImport("winmm.dll")] static extern uint joyGetPosEx(uint id, ref JOYINFOEX info);
    [DllImport("winmm.dll", CharSet = CharSet.Unicode)] static extern uint joyGetDevCapsW(UIntPtr id, out JOYCAPSW caps, uint size);
    const uint JOY_RETURNALL = 0xFF;

    /// <summary>A connected game controller as reported by the Windows joystick API.</summary>
    public class Device
    {
        public uint Id { get; init; }
        public ushort Vid { get; init; }
        public ushort Pid { get; init; }
        public string Name { get; init; } = "";
        public int Axes { get; init; }
        public int Buttons { get; init; }
        /// <summary>USB vendor:product identifier key used to match bindings.</summary>
        public string Key => $"{Vid:X4}:{Pid:X4}";
        /// <summary>Human-readable label with device name and key.</summary>
        public string Label => $"{Name}  ({Key})";
    }

    /// <summary>
    /// Enumerates all connected game controllers (up to 16 devices) using the Windows joystick API.
    /// Attempts to resolve the real device name from the registry OEM entries.
    /// </summary>
    public static List<Device> List()
    {
        var list = new List<Device>();
        for (uint id = 0; id < 16; id++)
        {
            var st = new JOYINFOEX { dwSize = (uint)Marshal.SizeOf<JOYINFOEX>(), dwFlags = JOY_RETURNALL };
            if (joyGetPosEx(id, ref st) != 0) continue;
            if (joyGetDevCapsW((UIntPtr)id, out var caps, (uint)Marshal.SizeOf<JOYCAPSW>()) != 0) continue;
            list.Add(new Device { Id = id, Vid = caps.wMid, Pid = caps.wPid, Name = OemName(caps.wMid, caps.wPid) ?? caps.szPname, Axes = (int)caps.wNumAxes, Buttons = (int)caps.wNumButtons });
        }
        return list;
    }

    /// <summary>The device's real name ("Logitech G HUB G920 Driving Force Racing Wheel USB"): winmm itself reports a driver name.</summary>
    static string? OemName(ushort vid, ushort pid)
    {
        var key = $@"System\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_{vid:X4}&PID_{pid:X4}";
        foreach (var root in new[] { Registry.CurrentUser, Registry.LocalMachine })
            try { if (root.OpenSubKey(key)?.GetValue("OEMName") is string s && s.Length > 0) return s; } catch (Exception) { }
        return null;
    }

    /// <summary>Reads the current state of a joystick; false when it is not connected.</summary>
    public static bool Read(uint id, out JOYINFOEX st)
    {
        st = new JOYINFOEX { dwSize = (uint)Marshal.SizeOf<JOYINFOEX>(), dwFlags = JOY_RETURNALL };
        return joyGetPosEx(id, ref st) == 0;
    }

    /// <summary>The value (0-65535) of axis i (X, Y, Z, R, U, V).</summary>
    public static uint Axis(in JOYINFOEX s, int i) => i switch { 0 => s.dwXpos, 1 => s.dwYpos, 2 => s.dwZpos, 3 => s.dwRpos, 4 => s.dwUpos, _ => s.dwVpos };
    public static readonly string[] AxisNames = { "X", "Y", "Z", "R", "U", "V" };
}
