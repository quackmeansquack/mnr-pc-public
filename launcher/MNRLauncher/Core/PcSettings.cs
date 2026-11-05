using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace MNRLauncher.Core;

/// <summary>The game's own PC settings, MNR\pc_settings.ini (host.c mnr_settings_path): [voice] push_to_talk, ptt_key,
/// mic (part of the device name), mic_boost (percent); [audio] volume (percent). The game also changes them itself
/// (F8 picks the next microphone, - and = the volume), so they are read fresh each time.</summary>
public static class PcSettings
{
    public static string File => Path.Combine(Paths.Root, "pc_settings.ini");

    [DllImport("kernel32", CharSet = CharSet.Unicode)] static extern int GetPrivateProfileString(string s, string k, string d, StringBuilder r, int n, string f);
    [DllImport("kernel32", CharSet = CharSet.Unicode)] static extern bool WritePrivateProfileString(string s, string k, string? v, string f);

    /// <summary>Reads a string value from pc_settings.ini using the Windows API.</summary>
    public static string Get(string section, string key, string def)
    {
        var sb = new StringBuilder(512); GetPrivateProfileString(section, key, def, sb, sb.Capacity, File); return sb.ToString();
    }
    /// <summary>Reads an integer value from pc_settings.ini, returning <paramref name="def"/> on parse failure.</summary>
    public static int GetInt(string section, string key, int def) => int.TryParse(Get(section, key, def.ToString()), out var v) ? v : def;
    /// <summary>Writes a value to pc_settings.ini using the Windows API.</summary>
    public static void Set(string section, string key, string? value) => WritePrivateProfileString(section, key, value, File);

    // microphones as the game lists them (voice.c: waveIn, names up to 31 characters)
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    /// <summary>WinMM recording-device capabilities structure (waveInGetDevCaps).</summary>
    struct WaveInCaps { public ushort Mid, Pid; public uint DriverVersion; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Name; public uint Formats; public ushort Channels, Reserved; }
    [DllImport("winmm.dll")] static extern uint waveInGetNumDevs();
    [DllImport("winmm.dll", CharSet = CharSet.Ansi)] static extern uint waveInGetDevCapsA(UIntPtr id, ref WaveInCaps caps, uint size);
    /// <summary>
    /// Enumerates all available microphones using the Windows multimedia API (waveInGetDevCapsA).
    /// Returns a list of device names as the game sees them.
    /// </summary>
    public static List<string> Microphones()
    {
        var list = new List<string>();
        try
        {
            for (uint i = 0, n = waveInGetNumDevs(); i < n; i++)
            {
                var c = new WaveInCaps();
                if (waveInGetDevCapsA((UIntPtr)i, ref c, (uint)Marshal.SizeOf<WaveInCaps>()) == 0) list.Add(c.Name);
            }
        }
        catch (Exception) { }
        return list;
    }

    /// <summary>
    /// Checks whether the voice chat codec (libcelpenc.prx / libcelpdec.prx) is ready in MNR\firmware,
    /// or if a PS3UPDAT.PUP file is available to extract it. Returns (state, detail) for UI display.
    /// </summary>
    public static (string state, string detail) VoiceCodec()
    {
        var fw = Path.Combine(Paths.Root, "firmware");
        if (System.IO.File.Exists(Path.Combine(fw, "libcelpenc.prx")) && System.IO.File.Exists(Path.Combine(fw, "libcelpdec.prx")))
            return ("ready", "The voice codec is ready.");
        foreach (var pup in UpdateFileCandidates())
            if (System.IO.File.Exists(pup)) return ("found", $"Found {pup}: the codec is taken from it the next time the game starts.");
        return ("missing", "Voice chat needs the PS3 system update file (PS3UPDAT.PUP, from playstation.com). Put it in the MNR folder or pick it here.");
    }
    /// <summary>
    /// Ordered list of locations to search for a PS3UPDAT.PUP file: extracted disc, MNR root, MNR\firmware, Downloads.
    /// </summary>
    public static IEnumerable<string> UpdateFileCandidates()
    {
        yield return Path.Combine(Paths.Root, "extracted", "PS3_UPDATE", "PS3UPDAT.PUP");   // the disc's own
        yield return Path.Combine(Paths.Root, "PS3UPDAT.PUP");
        yield return Path.Combine(Paths.Root, "firmware", "PS3UPDAT.PUP");
        yield return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads", "PS3UPDAT.PUP");
    }
}
