using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Threading.Tasks;

namespace MNRLauncher.Core;

/// <summary>Synchronizes published creations (Local/Upload/Download folders) with the currently selected server.
///
/// Before the game goes online, this ensures the published creations on disk match the server it will play on,
/// so the server's validation check doesn't delete anything. The game queries player_creations/&lt;id&gt;.xml
/// for each published creation; a stock server returns "ban" for ids it doesn't know, which the game then deletes.
///
/// What it does:
///  1. Switches servers: each server has its own published creations (Data\Upload and Data\Download). Switching
///     servers sets the previous server's aside in launcher\data\server_creations\&lt;server&gt; and brings
///     back the new one's.
///  2. Reinstalls old downloads: downloads from the first launcher versions were placed in Data\Local as the
///     game's own creations; these are reinstalled as proper downloads (Data\Download).
///  3. Imports missing creations: a download the server doesn't have is imported via the mnrpc import endpoint
///     (servers with this capability, like PLGarage/mnrpc/caps). One the server still can't have is set aside
///     in launcher\data\server_creations\_unavailable.
///  4. Restores local originals: one's own shared creation the server doesn't have (its database was reset, say)
///     becomes a local creation again (a free id from 5000 to 9999, published state 0), so it can be shared
///     again in the game.
///
/// Offline (or when the server can't be reached) nothing is changed: the game checks creations only online.</summary>
public static class CreationSync
{
    static readonly HttpClient Quick = new() { Timeout = TimeSpan.FromSeconds(6) };
    static string DataDir => Path.Combine(Paths.CreationsDir, "1", "DATA");
    static string StashDir => Path.Combine(Paths.LauncherData, "server_creations");
    static readonly string[] Published = { "UPLOAD", "DOWNLOAD" };
    static readonly string[] TypeFolders = { "Vehicle", "Track", "Character" };   // by the .CMD's type (0x84)

    /// <summary>Creates a filesystem-safe key from a server address: lowercase, alphanumeric and dots preserved,
    /// everything else replaced with underscores. Used as the folder name under server_creations/.</summary>
    static string Key(string address) => string.Concat(address.Trim().ToLowerInvariant().Select(c => char.IsLetterOrDigit(c) || c == '.' ? c : '_'));
    static uint U32(byte[] c, int o) => (uint)(c[o] << 24 | c[o + 1] << 16 | c[o + 2] << 8 | c[o + 3]);
    static string Name(string stem) { var f = Path.GetFileName(stem); return f.Length > 9 && f[8] == '_' ? f[9..] : f; }   // (without the id)

    /// <summary>Checks the server, switches published creations if needed, reinstalls old downloads, imports missing
    /// creations, and restores local originals. Returns a list of lines describing what changed (empty when nothing did).</summary>
    public static async Task<List<string>> Prepare(Settings s, Action<string>? status = null)
    {
        var notes = new List<string>();
        if (!s.OnlineEnabled) return notes;
        var server = CreationDownload.ServerBase(s);
        try { using var r = await Quick.GetAsync($"{server}/player_creations/1.xml"); } catch (Exception) { return notes; }   // unreachable: leave everything
        var key = Key(s.ServerAddress.Length > 0 ? s.ServerAddress : CreationDownload.RevivalAddress);

        // 1. each server's own published creations
        if (s.CreationsServer.Length > 0 && s.CreationsServer != key)
        {
            status?.Invoke("Switching your creations to this server…");
            int away = MoveTree(DataDir, Path.Combine(StashDir, s.CreationsServer)), back = MoveTree(Path.Combine(StashDir, key), DataDir);
            if (away + back > 0) notes.Add($"Creations from the other server were set aside ({away}); this server's came back ({back}).");
        }
        if (s.CreationsServer != key) { s.CreationsServer = key; s.Save(); }

        // 2. downloads the first launchers installed as local creations
        var old = CreationInstaller.OldInstalls();
        foreach (var (revivalId, stem) in old)
        {
            status?.Invoke("Reinstalling downloaded creations…");
            try { await CreationDownload.Install(s, revivalId); CreationInstaller.ForgetOldInstall(revivalId.ToString(), stem); notes.Add($"Reinstalled {Name(stem)} as a download."); }
            catch (Exception) { /* not available on this server: left as it was */ }
        }

        // 3. downloads and 4. one's own shared creations, checked against this server
        bool canImport = !CreationDownload.OnRevival(s) && await CreationDownload.CanImport(server);
        foreach (var area in Published)
            foreach (var cmdFile in Directory.Exists(Path.Combine(DataDir, area)) ? Directory.GetFiles(Path.Combine(DataDir, area), "*.CMD", SearchOption.AllDirectories) : Array.Empty<string>())
            {
                byte[] cmd; try { cmd = File.ReadAllBytes(cmdFile); } catch (Exception) { continue; }
                if (cmd.Length < 0xB8) continue;
                int id = (int)U32(cmd, 0x60);
                status?.Invoke("Checking your creations with the server…");
                var stem = cmdFile[..^4];
                if (await Known(server, id))
                {   // a download named as the game names it (after the creation's own name, not the server's listing)
                    var want = $"{id:X8}_{CreationDownload.FileName(CreationDownload.NameInCmd(cmd))}";
                    if (area == "DOWNLOAD" && !string.Equals(Path.GetFileName(stem), want, StringComparison.OrdinalIgnoreCase)) RenameStem(stem, want);   // (case alone: the same file on Windows)
                    continue;
                }
                bool hasData = File.Exists(stem + ".SAV") || File.Exists(stem + ".TRK");
                if (area == "DOWNLOAD")
                {
                    if (canImport && await CreationDownload.Import(server, id) && await Known(server, id)) { notes.Add($"Added {Name(stem)} to your server."); continue; }
                    MoveStem(stem, Path.Combine(StashDir, "_unavailable", Path.GetRelativePath(DataDir, Path.GetDirectoryName(stem)!)));
                    notes.Add($"{Name(stem)} isn't on this server: set aside (launcher\\data\\server_creations\\_unavailable).");
                }
                else if (!hasData)
                {   // a published track keeps only its info and pictures in Upload (its data stays with the local original)
                    MoveStem(stem, Path.Combine(StashDir, "_unavailable", Path.GetRelativePath(DataDir, Path.GetDirectoryName(stem)!)));
                    notes.Add($"{Name(stem)} (shared, its data kept with your original) isn't on this server: its listing was set aside; the original is in your creations.");
                }
                else
                {
                    try { ToLocal(stem, cmd); notes.Add($"{Name(stem)} isn't on this server any more: it's in your creations again, to share."); }
                    catch (Exception) { }
                }
            }
        return notes;
    }

    /// <summary>Checks if the server has a record of the given creation id. Returns true if the server knows about it
    /// or if the server query fails (no answer: assume it exists, change nothing).</summary>
    static async Task<bool> Known(string server, int id)
    {
        try { return await CreationDownload.Record(server, id) != null; } catch (Exception) { return true; }   // (no answer: assume so, change nothing)
    }

    /// <summary>Converts a published creation back to local: assigns a free local id (5000-9999), sets published state
    /// to 0, re-signs the .CMD, and moves all files to the LOCAL folder. Throws if no free local id is available.</summary>
    static void ToLocal(string stem, byte[] cmd)
    {
        var used = new HashSet<uint>();
        foreach (var f in Directory.Exists(Path.Combine(DataDir, "LOCAL")) ? Directory.GetFiles(Path.Combine(DataDir, "LOCAL"), "*.CMD", SearchOption.AllDirectories) : Array.Empty<string>())
            try { var c = File.ReadAllBytes(f); if (c.Length >= 0x64) used.Add(U32(c, 0x60)); } catch (Exception) { }
        uint id = 5000; while (used.Contains(id)) id++;
        if (id > 9999) throw new IOException("No free local creation id.");
        var n = (byte[])cmd.Clone();
        n[0x60] = (byte)(id >> 24); n[0x61] = (byte)(id >> 16); n[0x62] = (byte)(id >> 8); n[0x63] = (byte)id;
        n[0x88] = n[0x89] = n[0x8A] = n[0x8B] = 0;
        var check = CreationDownload.CmdCheck(n);
        for (int i = 0; i < 8; i++) n[0x58 + i] = (byte)(check >> (56 - 8 * i));
        uint type = U32(cmd, 0x84);
        var dir = Path.Combine(DataDir, "LOCAL", type < 3 ? TypeFolders[type] : "Character");
        Directory.CreateDirectory(dir);
        var name = Name(stem);
        var dest = Path.Combine(dir, $"{id:X8}_{name}");
        foreach (var f in Directory.GetFiles(Path.GetDirectoryName(stem)!, Path.GetFileName(stem) + "*"))
        {
            var rest = Path.GetFileName(f)[Path.GetFileName(stem).Length..];
            if (rest.Equals(".CMD", StringComparison.OrdinalIgnoreCase)) { File.WriteAllBytes(dest + ".CMD", n); File.Delete(f); }
            else File.Move(f, dest + rest, true);
        }
    }

    /// <summary>Renames all files matching the stem's filename to use the new name, preserving extensions.</summary>
    static void RenameStem(string stem, string newName)
    {
        var dir = Path.GetDirectoryName(stem)!; var old = Path.GetFileName(stem);
        foreach (var f in Directory.GetFiles(dir, old + "*")) File.Move(f, Path.Combine(dir, newName + Path.GetFileName(f)[old.Length..]), true);
    }

    /// <summary>Moves all files matching the stem's filename to the target directory.</summary>
    static void MoveStem(string stem, string toDir)
    {
        Directory.CreateDirectory(toDir);
        foreach (var f in Directory.GetFiles(Path.GetDirectoryName(stem)!, Path.GetFileName(stem) + "*")) File.Move(f, Path.Combine(toDir, Path.GetFileName(f)), true);
    }

    /// <summary>Moves the files of from\UPLOAD and from\DOWNLOAD (kept by type folder) to the same places under to.
    /// Returns how many creations moved.</summary>
    static int MoveTree(string from, string to)
    {
        int n = 0;
        foreach (var area in Published)
        {
            var src = Path.Combine(from, area);
            if (!Directory.Exists(src)) continue;
            foreach (var f in Directory.GetFiles(src, "*", SearchOption.AllDirectories))
            {
                var dest = Path.Combine(to, area, Path.GetRelativePath(src, f));
                Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                File.Move(f, dest, true);
                if (f.EndsWith(".CMD", StringComparison.OrdinalIgnoreCase)) n++;
            }
        }
        return n;
    }
}
