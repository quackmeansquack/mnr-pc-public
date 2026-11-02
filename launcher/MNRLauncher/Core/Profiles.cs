using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace MNRLauncher.Core;

/// <summary>A save profile: a folder of save data the game uses instead of the normal one.</summary>
public class Profile
{
    public string Name { get; init; } = "";
    public string Folder { get; init; } = "";
    public bool IsDefault { get; init; }
    public bool Active { get; set; }
    public string DisplayName => IsDefault ? "Default (the game's normal saves)" : Name;
    public string Summary
    {
        get
        {
            if (!Directory.Exists(Folder)) return "empty: the game starts from the beginning";
            var files = Directory.EnumerateFiles(Folder, "*", SearchOption.AllDirectories).ToList();
            if (files.Count == 0) return "empty: the game starts from the beginning";
            var last = files.Max(File.GetLastWriteTime);
            return $"{files.Count} files  ·  {files.Sum(f => new FileInfo(f).Length) / 1024.0:0} KB  ·  last saved {last:yyyy-MM-dd HH:mm}";
        }
    }
    public string Badge => Active ? "IN USE" : "";
}

/// <summary>Save profiles ("instances") for switching between saves, e.g. a fresh game for testing the intro next to
/// a finished career. Each lives in MNR\launcher\data\profiles\&lt;name&gt; and is passed to the game as MNR_SAVEDATA_DIR
/// (the folder that holds the save directories, like hdd0\home\00000001\savedata). Creations and installed game data
/// are shared by all profiles.</summary>
public static class Profiles
{
    public static string Dir => Path.Combine(Paths.LauncherData, "profiles");
    public static string DefaultFolder => Path.Combine(Paths.Hdd0, "home", "00000001", "savedata");

    public static List<Profile> List(Settings s)
    {
        Directory.CreateDirectory(Dir);
        var list = new List<Profile> { new() { Name = "", Folder = DefaultFolder, IsDefault = true } };
        list.AddRange(Directory.GetDirectories(Dir).OrderBy(d => d, StringComparer.OrdinalIgnoreCase).Select(d => new Profile { Name = Path.GetFileName(d), Folder = d }));
        foreach (var p in list) p.Active = p.Name == (s.ActiveProfile ?? "");
        if (!list.Any(p => p.Active)) list[0].Active = true;   // a deleted profile falls back to Default
        return list;
    }

    public static Profile Active(Settings s) => List(s).First(p => p.Active);

    public static string Create(string name, Profile? copyFrom)
    {
        var dest = Path.Combine(Dir, ModManager.SafeName(name));
        if (Directory.Exists(dest)) throw new IOException($"A profile called “{Path.GetFileName(dest)}” already exists.");
        Directory.CreateDirectory(dest);
        if (copyFrom != null && Directory.Exists(copyFrom.Folder)) CopyTree(copyFrom.Folder, dest);
        return dest;
    }

    public static void Rename(Profile p, string name, Settings s)
    {
        if (p.IsDefault) throw new InvalidOperationException("The default profile can't be renamed.");
        var dest = Path.Combine(Dir, ModManager.SafeName(name));
        if (Directory.Exists(dest)) throw new IOException($"A profile called “{Path.GetFileName(dest)}” already exists.");
        Directory.Move(p.Folder, dest);
        if (s.ActiveProfile == p.Name) s.ActiveProfile = Path.GetFileName(dest);
    }

    public static void Delete(Profile p, Settings s)
    {
        if (p.IsDefault) throw new InvalidOperationException("The default profile can't be deleted.");
        Directory.Delete(p.Folder, true);
        if (s.ActiveProfile == p.Name) s.ActiveProfile = "";
    }

    static void CopyTree(string from, string to)
    {
        foreach (var d in Directory.GetDirectories(from, "*", SearchOption.AllDirectories)) Directory.CreateDirectory(Path.Combine(to, Path.GetRelativePath(from, d)));
        foreach (var f in Directory.GetFiles(from, "*", SearchOption.AllDirectories)) File.Copy(f, Path.Combine(to, Path.GetRelativePath(from, f)), true);
    }
}
