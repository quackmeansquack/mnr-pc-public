using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;

namespace MNRLauncher.Core;

/// <summary>Turns a downloaded creation (data.bin from the server) into the files the game loads.
///
/// data.bin ("CTH" container, the same thing the game downloads from PlayerConnect):
///   char[4] "CTH\x01"; u32 bodySize; u32 cmdSize (big-endian); body[bodySize]; cmd[cmdSize]
///   body: the creation itself, a QCMP-compressed .SAV (characters, karts) or .TRK (tracks)
///   cmd:  its metadata file (.CMD, magic BF DB 1A CD): the creation's own id (u32 at 0x60, given by its creator's
///         console) and its name (at 0x2C)
///
/// The game finds a creation by the id and name in its .CMD and loads it from
/// hdd0\game\BCUS98167_UCC\USRDIR\1\DATA\LOCAL\&lt;CHARACTER|VEHICLE|TRACK&gt;\&lt;id as 8 hex digits&gt;_&lt;name&gt;.SAV|.TRK
/// (selecting a downloaded WildDuck read Data\Local\Character\00001661_WildDuck.sav), with .CMD, .PNG and _SMALL.PNG
/// beside it, like the built-in ones in USRDIR\DATA\ROM. Files named by the online id under DOWNLOAD were listed but
/// couldn't be loaded. Which online creation went where is kept in launcher\data\installed.json.</summary>
public static class CreationInstaller
{
    public static string DataDir => Path.Combine(Paths.CreationsDir, "1", "DATA");
    static string IndexFile => Path.Combine(Paths.LauncherData, "installed.json");

    public record Parsed(byte[] Body, byte[] Cmd, uint LocalId, string Name);
    public record Entry(string Type, string Stem, bool RemixPatched = false);   // RemixPatched: launcher 2026.10.23.2245 changed its .CMD (UndoRemixPatches)

    public static Parsed Parse(byte[] d)
    {
        if (d.Length < 12 || d[0] != 'C' || d[1] != 'T' || d[2] != 'H') throw new InvalidDataException("Not a creation file (no CTH header).");
        uint body = BE(d, 4), cmd = BE(d, 8);
        if (12L + body + cmd > d.Length) throw new InvalidDataException("The creation file is cut short.");
        var b = d.AsSpan(12, (int)body).ToArray();
        var c = d.AsSpan(12 + (int)body, (int)cmd).ToArray();
        if (b.Length < 4 || Encoding.ASCII.GetString(b, 0, 4) != "QCMP") throw new InvalidDataException("Unexpected creation data (no QCMP).");
        var (id, name) = ParseCmd(c);
        return new Parsed(b, c, id, name);
    }

    public static (uint id, string name) ParseCmd(byte[] c)
    {
        if (c.Length < 0x64 || c[0] != 0xBF || c[1] != 0xDB || c[2] != 0x1A || c[3] != 0xCD) throw new InvalidDataException("Unexpected creation metadata.");
        // the name field runs to 0x46: up to 25 characters and a zero ("Sonic Kart - Transformed2" ends at 0x45; reading
        // only to 0x44 installed it as "...Transformed", a file the game asked for under the full name and never found)
        int end = Array.IndexOf(c, (byte)0, 0x2C, 0x46 - 0x2C); if (end < 0) end = 0x46;
        return (BE(c, 0x60), Encoding.Latin1.GetString(c, 0x2C, end - 0x2C));
    }

    static uint BE(byte[] d, int o) => (uint)(d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]);

    public static string Folder(string apiType) => apiType switch { "CHARACTER" => "CHARACTER", "KART" => "VEHICLE", "TRACK" => "TRACK", _ => throw new InvalidDataException($"Unknown creation type {apiType}.") };

    /// <summary>The name as the game spells it in the path; only characters Windows can't store are replaced.</summary>
    static string FileName(string name)
    {
        var bad = Path.GetInvalidFileNameChars();
        var s = new string(name.Select(ch => bad.Contains(ch) ? '_' : ch).ToArray()).Trim();
        return s.Length == 0 ? "Creation" : s;
    }

    static Dictionary<string, Entry> LoadIndex()
    {
        try { if (File.Exists(IndexFile)) return JsonSerializer.Deserialize<Dictionary<string, Entry>>(File.ReadAllText(IndexFile)) ?? new(); }
        catch (Exception) { }
        return new();
    }
    static void SaveIndex(Dictionary<string, Entry> ix)
    {
        Directory.CreateDirectory(Paths.LauncherData);
        File.WriteAllText(IndexFile, JsonSerializer.Serialize(ix, new JsonSerializerOptions { WriteIndented = true }));
    }

    /// <summary>The installed files' common stem (without extension), if this creation is installed.</summary>
    public static string? Installed(RemoteCreation c)
    {
        var ix = LoadIndex();
        return ix.TryGetValue(c.Id.ToString(), out var e) && File.Exists(e.Stem + ".CMD") ? e.Stem : null;
    }

    /// <summary>Installs from a launcher download folder (data.bin, preview.png, preview_small.png). Returns the stem.</summary>
    public static string Install(RemoteCreation c, string downloadDir)
    {
        var p = Parse(File.ReadAllBytes(Path.Combine(downloadDir, "data.bin")));
        var dir = Path.Combine(DataDir, "LOCAL", Folder(c.Type));
        Directory.CreateDirectory(dir);
        var ix = LoadIndex();
        // another creation already using this id (one of the player's own, or another download)?
        var mine = ix.TryGetValue(c.Id.ToString(), out var old) ? old.Stem : null;
        foreach (var other in Directory.GetFiles(dir, $"{p.LocalId:X8}_*.CMD"))
            if (!string.Equals(other[..^4], mine, StringComparison.OrdinalIgnoreCase))
                throw new IOException($"“{Path.GetFileNameWithoutExtension(other)}” already uses this creation's id ({p.LocalId:X8}); installing would replace it.");
        Remove(c);   // an older version (maybe under another name)
        var stem = Path.Combine(dir, $"{p.LocalId:X8}_{FileName(p.Name.Length > 0 ? p.Name : c.Name)}");
        File.WriteAllBytes(stem + (c.Type == "TRACK" ? ".TRK" : ".SAV"), p.Body);
        File.WriteAllBytes(stem + ".CMD", p.Cmd);
        var big = Path.Combine(downloadDir, "preview.png"); var small = Path.Combine(downloadDir, "preview_small.png");
        if (File.Exists(big)) File.Copy(big, stem + ".PNG", true);
        if (File.Exists(big) || File.Exists(small)) WriteSmall(File.Exists(big) ? big : small, stem + "_SMALL.PNG");
        ix = LoadIndex(); ix[c.Id.ToString()] = new Entry(c.Type, stem); SaveIndex(ix);
        return stem;
    }

    /// <summary>Launcher 2026.10.23.2245 set bit 0x80 of the .CMD byte at 0x8C on downloads to allow remixing; the game's
    /// permission doesn't come from there (the studios' checks are overridden in the game instead, MNR_ALLOW_REMIX), so
    /// those bytes are put back. Returns how many.</summary>
    const int RemixByte = 0x8C; const byte RemixBit = 0x80;
    public static int UndoRemixPatches()
    {
        var ix = LoadIndex(); int n = 0;
        foreach (var (key, e) in ix.ToList())
        {
            if (!e.RemixPatched) continue;
            var cmd = e.Stem + ".CMD";
            try
            {
                if (File.Exists(cmd))
                {
                    var d = File.ReadAllBytes(cmd);
                    if (d.Length > RemixByte && (d[RemixByte] & RemixBit) != 0) { d[RemixByte] &= unchecked((byte)~RemixBit); File.WriteAllBytes(cmd, d); }
                }
                ix[key] = e with { RemixPatched = false }; n++;
            }
            catch (Exception) { /* leave a file we can't read or write as it is */ }
        }
        if (n > 0) SaveIndex(ix);
        return n;
    }

    /// <summary>Downloads the first launchers installed in Data\Local (as if one's own creations, under their
    /// creators' console ids, which the game then rejected online): their Revival ids and files. CreationSync installs
    /// them again as the game's own downloads (CreationDownload) and then forgets these.</summary>
    public static List<(int revivalId, string stem)> OldInstalls() =>
        LoadIndex().Where(kv => int.TryParse(kv.Key, out _) && File.Exists(kv.Value.Stem + ".CMD"))
                   .Select(kv => (int.Parse(kv.Key), kv.Value.Stem)).ToList();
    public static void ForgetOldInstall(string key, string stem)
    {
        foreach (var ext in new[] { ".SAV", ".TRK", ".CMD", ".PNG", "_SMALL.PNG", ".STAT" })
            if (File.Exists(stem + ext)) File.Delete(stem + ext);
        var ix = LoadIndex(); ix.Remove(key); SaveIndex(ix);
    }
    public static void WriteSmallFrom(string big, string small) => WriteSmall(big, small);

    /// <summary>The game's menus draw _SMALL.PNG into a 128x128 slot (every thumbnail the game makes is that size);
    /// the site's 64x64 preview showed as a quarter-size picture in the corner. Scaled from the full preview.</summary>
    public const int SmallSize = 128;
    static void WriteSmall(string from, string to)
    {
        var src = new System.Windows.Media.Imaging.BitmapImage();
        src.BeginInit(); src.CacheOption = System.Windows.Media.Imaging.BitmapCacheOption.OnLoad; src.UriSource = new Uri(from); src.EndInit();
        System.Windows.Media.Imaging.BitmapSource bmp = new System.Windows.Media.Imaging.FormatConvertedBitmap(src, System.Windows.Media.PixelFormats.Bgra32, null, 0);
        if (bmp.PixelWidth != SmallSize || bmp.PixelHeight != SmallSize)
            bmp = new System.Windows.Media.Imaging.TransformedBitmap(bmp, new System.Windows.Media.ScaleTransform((double)SmallSize / bmp.PixelWidth, (double)SmallSize / bmp.PixelHeight));
        var enc = new System.Windows.Media.Imaging.PngBitmapEncoder();
        enc.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bmp));
        using var f = File.Create(to);
        enc.Save(f);
    }

    /// <summary>Redoes 64x64 small thumbnails left by earlier launcher versions. Returns how many.</summary>
    public static int FixSmallThumbnails()
    {
        int n = 0;
        foreach (var e in LoadIndex().Values)
        {
            string small = e.Stem + "_SMALL.PNG", big = e.Stem + ".PNG";
            if (!File.Exists(small) || !File.Exists(big)) continue;
            try
            {
                var d = System.Windows.Media.Imaging.BitmapDecoder.Create(new Uri(small), System.Windows.Media.Imaging.BitmapCreateOptions.None, System.Windows.Media.Imaging.BitmapCacheOption.OnLoad);
                if (d.Frames[0].PixelWidth == SmallSize) continue;
                WriteSmall(big, small); n++;
            }
            catch (Exception) { }
        }
        return n;
    }

    /// <summary>Renames downloads that earlier launcher versions installed under a name cut to 24 characters, so their
    /// files carry the full name the game asks for. Returns how many.</summary>
    public static int FixCutNames()
    {
        var ix = LoadIndex(); int n = 0;
        foreach (var (key, e) in ix.ToList())
        {
            try
            {
                if (!File.Exists(e.Stem + ".CMD")) continue;
                var (id, name) = ParseCmd(File.ReadAllBytes(e.Stem + ".CMD"));
                var stem = Path.Combine(Path.GetDirectoryName(e.Stem)!, $"{id:X8}_{FileName(name)}");
                if (string.Equals(stem, e.Stem, StringComparison.OrdinalIgnoreCase) || File.Exists(stem + ".CMD")) continue;
                foreach (var ext in new[] { ".SAV", ".TRK", ".CMD", ".PNG", "_SMALL.PNG", ".STAT" })
                    if (File.Exists(e.Stem + ext)) File.Move(e.Stem + ext, stem + ext);
                ix[key] = e with { Stem = stem }; n++;
            }
            catch (Exception) { /* leave files we can't read or rename as they are */ }
        }
        if (n > 0) SaveIndex(ix);
        return n;
    }

    /// <summary>Looks over every creation the game keeps (LOCAL, UPLOAD, DOWNLOAD; characters, karts, tracks): renames
    /// cut-short downloads and redoes small thumbnails, and reports what it can't fix (missing data or pictures, a name
    /// whose id doesn't match its .CMD, two creations sharing an id). One line per finding; the first is a summary.</summary>
    public static List<string> Check()
    {
        var notes = new List<string>();
        int renamed = 0, thumbs = 0, n = 0;
        try { renamed = FixCutNames(); } catch (Exception) { }
        try { thumbs = FixSmallThumbnails(); } catch (Exception) { }
        if (renamed > 0) notes.Add($"Renamed {renamed} download(s) to their full names.");
        if (thumbs > 0) notes.Add($"Remade {thumbs} small menu picture(s).");
        foreach (var area in new[] { "LOCAL", "UPLOAD", "DOWNLOAD" })
            foreach (var folder in new[] { "CHARACTER", "VEHICLE", "TRACK" })
            {
                var dir = Path.Combine(DataDir, area, folder);
                if (!Directory.Exists(dir)) continue;
                var byId = new Dictionary<uint, List<string>>();
                foreach (var cmd in Directory.GetFiles(dir, "*.CMD"))
                {
                    n++;
                    var stem = cmd[..^4]; var file = Path.GetFileName(stem); var where = $"{area}\\{folder}\\{file}";
                    if (file.Length < 10 || file[8] != '_' || !uint.TryParse(file[..8], System.Globalization.NumberStyles.HexNumber, null, out var fid))
                    { notes.Add($"{where}: the name doesn't start with an id, so the game can't find it."); continue; }
                    try { var (id, _) = ParseCmd(File.ReadAllBytes(cmd)); if (id != fid) notes.Add($"{where}: named {fid:X8} but its .CMD says {id:X8}."); }
                    catch (Exception) { notes.Add($"{where}: its .CMD (the creation's info) is damaged."); }
                    // (a published track keeps only its info and pictures in UPLOAD: its data stays in LOCAL)
                    if (!(area == "UPLOAD" && folder == "TRACK") && !File.Exists(stem + (folder == "TRACK" ? ".TRK" : ".SAV"))) notes.Add($"{where}: the creation itself ({(folder == "TRACK" ? ".TRK" : ".SAV")}) is missing.");
                    if (!File.Exists(stem + ".PNG")) notes.Add($"{where}: no picture (.PNG); it shows blank in the menus.");
                    var data = stem + (folder == "TRACK" ? ".TRK" : ".SAV");
                    if (area != "LOCAL" && File.Exists(data))   // the game's own checks of a published creation (CreationDownload)
                        try { if (!CreationDownload.Valid(File.ReadAllBytes(cmd), File.ReadAllBytes(data))) notes.Add($"{where}: its checksums don't match its contents; the game will delete it."); }
                        catch (Exception) { }
                    if (!byId.TryGetValue(fid, out var l)) byId[fid] = l = new();
                    l.Add(file);
                }
                foreach (var (id, files) in byId.Where(k => k.Value.Count > 1))
                    notes.Add($"{area}\\{folder}: {string.Join(" and ", files)} share the id {id:X8}; the game may load the wrong one.");
            }
        notes.Insert(0, $"Checked {n} creation(s).{(notes.Count == 0 ? " Everything is in order." : "")}");
        return notes;
    }

    public static void Remove(RemoteCreation c)
    {
        var ix = LoadIndex();
        if (ix.TryGetValue(c.Id.ToString(), out var e))
        {
            foreach (var ext in new[] { ".SAV", ".TRK", ".CMD", ".PNG", "_SMALL.PNG", ".STAT" })
                if (File.Exists(e.Stem + ext)) File.Delete(e.Stem + ext);
            ix.Remove(c.Id.ToString()); SaveIndex(ix);
        }
    }
}
