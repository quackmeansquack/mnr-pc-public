using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text.Json;

namespace MNRLauncher.Core;

/// <summary>One creation on disk: its data file(s) and any thumbnails with the same name.</summary>
public class Creation
{
    public string Name { get; init; } = "";
    public string Kind { get; init; } = "";          // Character, Kart, Track or Other
    public List<string> Files { get; init; } = new(); // relative to the creations folder
    public string? Thumbnail { get; init; }
    /// <summary>The thumbnail read into memory: an Image bound to the file path keeps the file open, and the game then
    /// can't rewrite it (its avatar pictures failed to save, which it reports as "not enough space in the HDD").</summary>
    public System.Windows.Media.ImageSource? ThumbnailImage
    {
        get
        {
            if (Thumbnail == null) return null;
            try
            {
                var b = new System.Windows.Media.Imaging.BitmapImage();
                using var fs = new FileStream(Thumbnail, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                b.BeginInit(); b.CacheOption = System.Windows.Media.Imaging.BitmapCacheOption.OnLoad; b.StreamSource = fs; b.DecodePixelWidth = 144; b.EndInit();
                b.Freeze();
                return b;
            }
            catch (Exception) { return null; }
        }
    }
    public long Bytes { get; init; }
    public DateTime Modified { get; init; }
    public string Source { get; init; } = "";   // Mine, Downloaded, Uploaded, DLC
    public string Summary => $"{(Source.Length > 0 ? Source + " " : "")}{Kind}  ·  {Files.Count} file{(Files.Count == 1 ? "" : "s")}  ·  {Bytes / 1024.0:0.#} KB  ·  {Modified:yyyy-MM-dd HH:mm}";
}

/// <summary>manifest.json inside a .mnrcreation file.</summary>
public class CreationManifest
{
    public string Format { get; set; } = "mnrcreation";
    public int Version { get; set; } = 1;
    public string Game { get; set; } = "BCUS98167";
    public string Name { get; set; } = "";
    public string Kind { get; set; } = "";
    public string Author { get; set; } = "";
    public string Description { get; set; } = "";
    public List<string> Files { get; set; } = new();
}

/// <summary>Creations live in hdd0\game\BCUS98167_UCC\USRDIR (the game's user-created-content data). A .mnrcreation file
/// is a zip of manifest.json plus the creation's files under their paths in that folder, so importing puts them back
/// where the game looks for them.</summary>
public static class CreationStore
{
    static readonly string[] DataExt = { ".SAV", ".TRK", ".MOD", ".KRT", ".CHR", ".BIN" };

    public static List<Creation> Scan()
    {
        var root = Paths.CreationsDir;
        var list = new List<Creation>();
        if (!Directory.Exists(root)) return list;
        var all = Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories).ToList();
        // group by folder + base name (PLAYER_0_PRIMARY.PNG and PLAYER_0_PRIMARY.SAV belong together)
        foreach (var g in all.GroupBy(f => Path.Combine(Path.GetDirectoryName(f)!, StemOf(f)), StringComparer.OrdinalIgnoreCase))
        {
            var files = g.ToList();
            bool hasData = files.Any(f => DataExt.Contains(Path.GetExtension(f).ToUpperInvariant()));
            var pngs = files.Where(f => Path.GetExtension(f).Equals(".png", StringComparison.OrdinalIgnoreCase)).ToList();
            var kind = KindOf(g.Key, files);
            if (!hasData && kind == "Other" && !g.Key.Contains("AVATARS", StringComparison.OrdinalIgnoreCase)) continue;
            list.Add(new Creation
            {
                Name = Path.GetFileName(g.Key),
                Source = SourceOf(g.Key),
                Kind = hasData ? kind : "Avatar",
                Files = files.Select(f => Path.GetRelativePath(root, f)).ToList(),
                Thumbnail = pngs.OrderByDescending(f => new FileInfo(f).Length).FirstOrDefault(),
                Bytes = files.Sum(f => new FileInfo(f).Length),
                Modified = files.Max(f => File.GetLastWriteTime(f)),
            });
        }
        return list.OrderByDescending(c => c.Modified).ToList();
    }

    /// <summary>"PLAYER_0_SECONDARY_SMALL" -> "PLAYER_0_SECONDARY": thumbnails group with their creation.</summary>
    static string StemOf(string f)
    {
        var s = Path.GetFileNameWithoutExtension(f);
        foreach (var suffix in new[] { "_SMALL", "_LARGE", "_THUMB", "_THUMBNAIL" })
            if (s.EndsWith(suffix, StringComparison.OrdinalIgnoreCase)) return s[..^suffix.Length];
        return s;
    }

    static string SourceOf(string key)
    {
        var k = key.ToUpperInvariant().Replace('/', '\\');
        return k.Contains(@"\DOWNLOAD\") ? "Downloaded" : k.Contains(@"\LOCAL\") ? "Mine" : k.Contains(@"\UPLOAD\") ? "Uploaded" : k.Contains(@"\DLC\") ? "DLC" : "";
    }

    static string KindOf(string key, List<string> files)
    {
        var k = key.ToUpperInvariant();
        if (files.Any(f => f.EndsWith(".TRK", StringComparison.OrdinalIgnoreCase)) || k.Contains("TRACK")) return "Track";
        if (k.Contains("VEHICLE") || k.Contains("KART")) return "Kart";
        if (k.Contains("CHARACTER") || k.Contains("CHAR")) return "Character";
        return "Other";
    }

    public static void Export(Creation c, string archive, string author, string description)
    {
        if (File.Exists(archive)) File.Delete(archive);
        using var zip = ZipFile.Open(archive, ZipArchiveMode.Create);
        var m = new CreationManifest { Name = c.Name, Kind = c.Kind, Author = author, Description = description, Files = c.Files.Select(f => f.Replace('\\', '/')).ToList() };
        var e = zip.CreateEntry("manifest.json");
        using (var w = new StreamWriter(e.Open())) w.Write(JsonSerializer.Serialize(m, new JsonSerializerOptions { WriteIndented = true }));
        foreach (var rel in c.Files) zip.CreateEntryFromFile(Path.Combine(Paths.CreationsDir, rel), "files/" + rel.Replace('\\', '/'), CompressionLevel.Optimal);
    }

    public static CreationManifest ReadManifest(string archive)
    {
        using var zip = ZipFile.OpenRead(archive);
        var e = zip.GetEntry("manifest.json") ?? throw new InvalidDataException("Not a creation file (no manifest.json).");
        using var r = new StreamReader(e.Open());
        var m = JsonSerializer.Deserialize<CreationManifest>(r.ReadToEnd(), new JsonSerializerOptions { PropertyNameCaseInsensitive = true })
                ?? throw new InvalidDataException("The manifest couldn't be read.");
        if (m.Format != "mnrcreation") throw new InvalidDataException("Not a ModNation Racers creation file.");
        return m;
    }

    /// <summary>Files the import would overwrite.</summary>
    public static List<string> Collisions(string archive)
    {
        var m = ReadManifest(archive);
        return m.Files.Where(f => File.Exists(Path.Combine(Paths.CreationsDir, f))).ToList();
    }

    public static CreationManifest Import(string archive)
    {
        var m = ReadManifest(archive);
        var root = Path.GetFullPath(Paths.CreationsDir);
        using var zip = ZipFile.OpenRead(archive);
        foreach (var e in zip.Entries.Where(e => e.FullName.StartsWith("files/") && !e.FullName.EndsWith("/")))
        {
            var target = Path.GetFullPath(Path.Combine(root, e.FullName["files/".Length..]));
            if (!target.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) continue;   // no ../ escapes
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            e.ExtractToFile(target, true);
        }
        return m;
    }
}
