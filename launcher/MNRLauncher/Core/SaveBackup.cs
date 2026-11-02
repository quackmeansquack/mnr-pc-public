using System;
using System.IO;
using System.IO.Compression;
using System.Linq;

namespace MNRLauncher.Core;

/// <summary>Zips of a save profile's folder in launcher\data\save_backups, made before an update is installed and
/// before switching to another profile (and on request). The newest 10 per profile are kept.</summary>
public static class SaveBackup
{
    public static string Dir => Path.Combine(Paths.LauncherData, "save_backups");
    const int Keep = 10;

    /// <summary>The zip made, or null when the profile has no saves yet.</summary>
    public static string? Make(Profile p, string reason)
    {
        if (!Directory.Exists(p.Folder) || !Directory.EnumerateFiles(p.Folder, "*", SearchOption.AllDirectories).Any()) return null;
        Directory.CreateDirectory(Dir);
        var name = p.IsDefault ? "default" : string.Concat(p.Name.Select(c => Path.GetInvalidFileNameChars().Contains(c) ? '_' : c));
        var zip = Path.Combine(Dir, $"{name}_{DateTime.Now:yyyy-MM-dd_HH-mm-ss}_{reason}.zip");
        ZipFile.CreateFromDirectory(p.Folder, zip, CompressionLevel.Optimal, false);
        foreach (var old in new DirectoryInfo(Dir).GetFiles($"{name}_*.zip").OrderByDescending(f => f.CreationTimeUtc).Skip(Keep))
            try { old.Delete(); } catch (Exception) { }
        return zip;
    }

    /// <summary>The profile in use, before something risky; failures don't stop what comes next.</summary>
    public static void Active(Settings s, string reason)
    {
        try { Make(Profiles.Active(s), reason); } catch (Exception) { }
    }
}
