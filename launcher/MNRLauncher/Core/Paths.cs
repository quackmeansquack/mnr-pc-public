using System;
using System.IO;

namespace MNRLauncher.Core;

/// <summary>Centralised filesystem layout. All launcher paths flow through this class so the rest of the code
/// never concatenates strings with magic folder names.</summary>
///
/// <para>Discovery algorithm (FindRoot):</para>
/// <list type="bullet">
///   <item><description>1. Check MNR_ROOT environment variable (for portable/alternative installs).</description></item>
///   <item><description>2. Walk up from the launcher's own directory looking for re\recomp (compiled game) or
///   extracted\PS3_GAME (extracted ISO). This supports the embedded launcher sitting inside the game folder.</description></item>
///   <item><description>3. Read root.txt from %APPDATA%\MNRLauncher (user-chosen install location for a standalone launcher).</description></item>
///   <item><description>4. Fall back to %UserProfile%\Games\MNR (fresh standalone launcher in Downloads, etc.).</description></item>
/// </list>
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

    /// <summary>Pointer file (%APPDATA%\MNRLauncher\root.txt) used when the launcher is portable/standalone and does
    /// not live inside the game folder. Set by SetupPage during installation.</summary>
    public static string RootPointerFile => Path.Combine(Updater.AppDataDir, "root.txt");

    /// <summary>Reads the user-chosen install folder from RootPointerFile. Returns null if the file is missing or empty.</summary>
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

    /// <summary>Writes the user-chosen install folder to RootPointerFile for future launcher sessions.</summary>
    public static void SetChosenRoot(string folder)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(RootPointerFile)!);
        File.WriteAllText(RootPointerFile, Path.GetFullPath(folder));
    }

    /// <summary>Quick check: both the compiled executable and the extracted game data directory must exist.</summary>
    public static bool Installed => File.Exists(GameExe) && Directory.Exists(GameData);

    /// <summary>Root of the recompiled build tree (re\recomp contains the native build; re\bin_v111 contains EBOOT.elf).</summary>
    public static string ReDir => Path.Combine(Root, "re");

    /// <summary>The compiled Windows executable (mnr.exe) produced by the llvm-mingw + Python recompilation pipeline.</summary>
    public static string GameExe => Path.Combine(ReDir, "recomp", "out_v111", "mnr.exe");

    /// <summary>CPU instruction set baseline the native build was compiled for. Read from march.txt beside mnr.exe.
    /// "haswell" = AVX2/FMA/BMI1/2/LZCNT/MOVBE (Intel 4th gen+ / AMD Ryzen+). Falls back to haswell if the file is missing.</summary>
    public static string GameExeMarch
    {
        get { try { var f = Path.Combine(Path.GetDirectoryName(GameExe)!, "march.txt"); return File.Exists(f) ? File.ReadAllText(f).Trim() : "haswell"; } catch (Exception) { return "haswell"; } }
    }

    /// <summary>Decrypted EBOOT.elf (1.11) extracted from the PS3 ISO, used as the boot parameter for mnr.exe.</summary>
    public static string Eboot => Path.Combine(ReDir, "bin_v111", "EBOOT.elf");

    /// <summary>Extracted PS3 game data directory (the ISO contents under extracted/PS3_GAME).</summary>
    public static string GameData => Path.Combine(Root, "extracted", "PS3_GAME");

    /// <summary>Virtual hdd0 partition (PS3 filesystem layout: hdd0/game/BCUS98167/USRDIR holds the native code assets).</summary>
    public static string Hdd0 => Path.Combine(Root, "hdd0");

    /// <summary>USRDIR of the installed game (BCUS98167) on hdd0 — contains the PS3 native binaries and data.</summary>
    public static string InstalledUsrdir => Path.Combine(Hdd0, "game", "BCUS98167", "USRDIR");

    /// <summary>USRDIR of the user-created content partition (BCUS98167_UCC) — stores player creations and avatars.</summary>
    public static string CreationsDir => Path.Combine(Hdd0, "game", "BCUS98167_UCC", "USRDIR");

    /// <summary>Folder where user-placed mods live. Each subfolder is a mod package.</summary>
    public static string ModsDir => Path.Combine(Root, "mods");

    /// <summary>Directory for game logs (last_run.txt, etc.).</summary>
    public static string LogsDir => Path.Combine(Root, "logs");

    /// <summary>Launcher-specific data directory (settings, downloads, etc.).</summary>
    public static string LauncherData => Path.Combine(Root, "launcher", "data");

    /// <summary>JSON settings file path.</summary>
    public static string SettingsFile => Path.Combine(LauncherData, "settings.json");

    /// <summary>Directory for temporary downloads (ISO, PKG, patches, etc.).</summary>
    public static string DownloadsDir => Path.Combine(LauncherData, "downloads");

    /// <summary>Creates the launcher's working directories if they don't already exist.</summary>
    public static void EnsureFolders()
    {
        foreach (var d in new[] { ModsDir, LogsDir, LauncherData, DownloadsDir })
            Directory.CreateDirectory(d);
    }
}
