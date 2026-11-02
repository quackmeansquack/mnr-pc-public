using System;
using System.IO;

namespace MNRLauncher.Core;

/// <summary>Where everything lives. The launcher sits in MNR\launcher\...; the MNR folder is found by walking up
/// until a folder contains re\recomp (the native build) or extracted\PS3_GAME (the game files).</summary>
public static class Paths
{
    public static string Root { get; } = FindRoot();

    static string FindRoot()
    {
        var env = Environment.GetEnvironmentVariable("MNR_ROOT");
        if (!string.IsNullOrEmpty(env) && Directory.Exists(env)) return Path.GetFullPath(env);
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null)
        {
            if (Directory.Exists(Path.Combine(dir.FullName, "re", "recomp")) || Directory.Exists(Path.Combine(dir.FullName, "extracted", "PS3_GAME")))
                return dir.FullName;
            dir = dir.Parent;
        }
        var chosen = ChosenRoot();   // a standalone launcher: the folder picked for the install
        if (chosen != null) return chosen;
        // a fresh standalone launcher (e.g. in Downloads): a default install folder, changeable in Install & updates
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Games", "MNR");
    }

    /// <summary>The install folder chosen in the Install page (%APPDATA%\MNRLauncher\root.txt), for a launcher that
    /// doesn't sit inside the install.</summary>
    public static string RootPointerFile => Path.Combine(Updater.AppDataDir, "root.txt");
    public static string? ChosenRoot()
    {
        try
        {
            if (!File.Exists(RootPointerFile)) return null;
            var p = File.ReadAllText(RootPointerFile).Trim();
            return p.Length > 0 ? Path.GetFullPath(p) : null;
        }
        catch (Exception) { return null; }
    }
    public static void SetChosenRoot(string folder)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(RootPointerFile)!);
        File.WriteAllText(RootPointerFile, Path.GetFullPath(folder));
    }
    /// <summary>True when the game is installed at Root.</summary>
    public static bool Installed => File.Exists(GameExe) && Directory.Exists(GameData);

    public static string ReDir => Path.Combine(Root, "re");
    /// <summary>The 1.11 native build (the final product).</summary>
    public static string GameExe => Path.Combine(ReDir, "recomp", "out_v111", "mnr.exe");
    /// <summary>Which CPU baseline the build at GameExe was compiled for (march.txt next to it): x86-64-v2 or haswell.</summary>
    public static string GameExeMarch
    {
        get { try { var f = Path.Combine(Path.GetDirectoryName(GameExe)!, "march.txt"); return File.Exists(f) ? File.ReadAllText(f).Trim() : "haswell"; } catch (Exception) { return "haswell"; } }
    }
    public static string Eboot => Path.Combine(ReDir, "bin_v111", "EBOOT.elf");
    public static string GameData => Path.Combine(Root, "extracted", "PS3_GAME");
    public static string Hdd0 => Path.Combine(Root, "hdd0");
    public static string InstalledUsrdir => Path.Combine(Hdd0, "game", "BCUS98167", "USRDIR");
    /// <summary>The game's user-created content (creations, avatars).</summary>
    public static string CreationsDir => Path.Combine(Hdd0, "game", "BCUS98167_UCC", "USRDIR");
    public static string ModsDir => Path.Combine(Root, "mods");
    public static string LogsDir => Path.Combine(Root, "logs");
    public static string LauncherData => Path.Combine(Root, "launcher", "data");
    public static string SettingsFile => Path.Combine(LauncherData, "settings.json");
    public static string DownloadsDir => Path.Combine(LauncherData, "downloads");

    public static void EnsureFolders()
    {
        foreach (var d in new[] { ModsDir, LogsDir, LauncherData, DownloadsDir })
            Directory.CreateDirectory(d);
    }
}
