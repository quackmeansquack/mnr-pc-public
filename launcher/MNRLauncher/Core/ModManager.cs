using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text.Json;

namespace MNRLauncher.Core;

/// <summary>mod.json at the top of a mod.</summary>
public class ModInfo
{
    public string Name { get; set; } = "";
    public string Version { get; set; } = "1.0";
    public string Author { get; set; } = "";
    public string Description { get; set; } = "";
}

/// <summary>A mod is a folder in MNR\mods:
///   mod.json      name, version, author, description
///   files\        replacement game files, laid out like the game's USRDIR (files\DATA\WORLD\THEMEDEFS.XML ...)
///   patches.txt   optional data patches (same format as MNR\patches\patches_v111.txt)
///   preview.png   optional picture for the launcher
/// A .mnrmod file is the same thing zipped, for sharing.</summary>
public class Mod
{
    public string Folder { get; init; } = "";
    public string Id => Path.GetFileName(Folder);
    public ModInfo Info { get; init; } = new();
    public bool Enabled { get; set; }
    public int FileCount { get; init; }
    public bool HasPatches => File.Exists(PatchFile);
    public string PatchFile => Path.Combine(Folder, "patches.txt");
    public string FilesDir => Path.Combine(Folder, "files");
    public string? Preview => File.Exists(Path.Combine(Folder, "preview.png")) ? Path.Combine(Folder, "preview.png") : null;
    public string DisplayName => string.IsNullOrWhiteSpace(Info.Name) ? Id : Info.Name;
    public string Summary
    {
        get
        {
            var parts = new List<string>();
            if (!string.IsNullOrWhiteSpace(Info.Author)) parts.Add("by " + Info.Author);
            parts.Add("v" + Info.Version);
            parts.Add(FileCount == 1 ? "1 file" : $"{FileCount} files");
            if (HasPatches) parts.Add("patches");
            return string.Join("  ·  ", parts);
        }
    }
}

public static class ModManager
{
    public static List<Mod> Scan(Settings settings)
    {
        Directory.CreateDirectory(Paths.ModsDir);
        var mods = new List<Mod>();
        foreach (var dir in Directory.GetDirectories(Paths.ModsDir))
        {
            var info = new ModInfo();
            var json = Path.Combine(dir, "mod.json");
            try { if (File.Exists(json)) info = JsonSerializer.Deserialize<ModInfo>(File.ReadAllText(json), JsonOpts) ?? info; } catch (Exception) { }
            var files = Path.Combine(dir, "files");
            int count = Directory.Exists(files) ? Directory.EnumerateFiles(files, "*", SearchOption.AllDirectories).Count() : 0;
            mods.Add(new Mod { Folder = dir, Info = info, FileCount = count, Enabled = settings.EnabledMods.Contains(Path.GetFileName(dir)) });
        }
        // enabled mods first, in load order; the rest by name
        return mods.OrderBy(m => m.Enabled ? settings.EnabledMods.IndexOf(m.Id) : int.MaxValue).ThenBy(m => m.DisplayName).ToList();
    }

    static readonly JsonSerializerOptions JsonOpts = new() { PropertyNameCaseInsensitive = true, WriteIndented = true };

    /// <summary>Unpacks a .mnrmod (zip) into MNR\mods. Returns the new mod's folder.</summary>
    public static string Import(string archive)
    {
        var name = SafeName(Path.GetFileNameWithoutExtension(archive));
        var dest = Unique(Path.Combine(Paths.ModsDir, name));
        using var zip = ZipFile.OpenRead(archive);
        // archives made by hand often wrap everything in one top folder: strip it
        string prefix = "";
        var tops = zip.Entries.Select(e => e.FullName.Replace('\\', '/').Split('/')[0]).Distinct().ToList();
        if (tops.Count == 1 && zip.Entries.All(e => e.FullName.Replace('\\', '/').Contains('/'))) prefix = tops[0] + "/";
        foreach (var e in zip.Entries)
        {
            var rel = e.FullName.Replace('\\', '/');
            if (prefix.Length > 0) rel = rel.Substring(prefix.Length);
            if (rel.Length == 0 || rel.EndsWith("/")) continue;
            var target = Path.GetFullPath(Path.Combine(dest, rel));
            if (!target.StartsWith(dest + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) continue;   // no ../ escapes
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            e.ExtractToFile(target, true);
        }
        return dest;
    }

    /// <summary>Zips a mod folder into a .mnrmod file for sharing.</summary>
    public static void Export(Mod mod, string archive)
    {
        if (File.Exists(archive)) File.Delete(archive);
        ZipFile.CreateFromDirectory(mod.Folder, archive, CompressionLevel.Optimal, false);
    }

    /// <summary>Creates an empty mod skeleton (for mod makers).</summary>
    public static string CreateEmpty(string name, string author)
    {
        var dest = Unique(Path.Combine(Paths.ModsDir, SafeName(name)));
        Directory.CreateDirectory(Path.Combine(dest, "files"));
        File.WriteAllText(Path.Combine(dest, "mod.json"), JsonSerializer.Serialize(new ModInfo { Name = name, Author = author, Description = "" }, JsonOpts));
        File.WriteAllText(Path.Combine(dest, "patches.txt"),
            "# Data patches, one per line: <byte|be16|be32|bef32> <hex address> <value>\n# (addresses in the 1.11 EBOOT's data, as in MNR\\patches\\patches_v111.txt)\n");
        return dest;
    }

    /// <summary>The environment the game needs to load the enabled mods.</summary>
    public static (string mods, string patches) Environment(IEnumerable<Mod> enabledInOrder)
    {
        var list = enabledInOrder.ToList();
        return (string.Join(";", list.Select(m => m.Folder)), string.Join(";", list.Where(m => m.HasPatches).Select(m => m.PatchFile)));
    }

    /// <summary>Files that more than one enabled mod replaces (the first in load order wins).</summary>
    public static List<string> Conflicts(IEnumerable<Mod> enabledInOrder)
    {
        var seen = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var conflicts = new List<string>();
        foreach (var m in enabledInOrder)
        {
            if (!Directory.Exists(m.FilesDir)) continue;
            foreach (var f in Directory.EnumerateFiles(m.FilesDir, "*", SearchOption.AllDirectories))
            {
                var rel = Path.GetRelativePath(m.FilesDir, f);
                if (seen.TryGetValue(rel, out var first)) conflicts.Add($"{rel}: {first} wins over {m.DisplayName}");
                else seen[rel] = m.DisplayName;
            }
        }
        return conflicts;
    }

    public static string SafeName(string s)
    {
        foreach (var c in Path.GetInvalidFileNameChars()) s = s.Replace(c, '_');
        s = s.Trim();
        return s.Length == 0 ? "mod" : s;
    }

    public static string Unique(string path)
    {
        if (!Directory.Exists(path) && !File.Exists(path)) return path;
        for (int i = 2; ; i++) { var p = $"{path} ({i})"; if (!Directory.Exists(p) && !File.Exists(p)) return p; }
    }
}
