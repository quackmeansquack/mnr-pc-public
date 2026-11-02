using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Threading.Tasks;

namespace MNRLauncher.Core;

/// <summary>Before the game goes online: makes the published creations on disk match the server it will play on, so
/// its check of them (a stock server answers "ban" for ids it doesn't know, and the game then deletes the creation)
/// loses nothing.
///  - Each server has its own published creations (Data\Upload and Data\Download): switching servers sets the previous
///    server's aside in launcher\data\server_creations\&lt;server&gt; and brings back the new one's.
///  - Downloads the first launchers put in Data\Local are installed again as the game's own downloads.
///  - A download the server doesn't have is imported (servers with the import, from MNR Revival); one it still can't
///    have is set aside in launcher\data\server_creations\_unavailable.
///  - One's own shared creation the server doesn't have (its database was reset, say) becomes a local creation again
///    (a free id from 5000 to 9999, published state 0), so it can be shared again in the game.
/// Offline (or when the server can't be reached) nothing is changed: the game checks creations only online.</summary>
public static class CreationSync
{
    static readonly HttpClient Quick = new() { Timeout = TimeSpan.FromSeconds(6) };
    static string DataDir => Path.Combine(Paths.CreationsDir, "1", "DATA");
    static string StashDir => Path.Combine(Paths.LauncherData, "server_creations");
    static readonly string[] Published = { "UPLOAD", "DOWNLOAD" };
    static readonly string[] TypeFolders = { "Vehicle", "Track", "Character" };   // by the .CMD's type (0x84)

    static string Key(string address) => string.Concat(address.Trim().ToLowerInvariant().Select(c => char.IsLetterOrDigit(c) || c == '.' ? c : '_'));
    static uint U32(byte[] c, int o) => (uint)(c[o] << 24 | c[o + 1] << 16 | c[o + 2] << 8 | c[o + 3]);
    static string Name(string stem) { var f = Path.GetFileName(stem); return f.Length > 9 && f[8] == '_' ? f[9..] : f; }   // (without the id)

    /// <summary>Lines saying what changed (empty when nothing did).</summary>
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

    static async Task<bool> Known(string server, int id)
    {
        try { return await CreationDownload.Record(server, id) != null; } catch (Exception) { return true; }   // (no answer: assume so, change nothing)
    }

    /// <summary>One's own published creation made local again: a free local id, published state 0, re-signed.</summary>
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

    static void RenameStem(string stem, string newName)
    {
        var dir = Path.GetDirectoryName(stem)!; var old = Path.GetFileName(stem);
        foreach (var f in Directory.GetFiles(dir, old + "*")) File.Move(f, Path.Combine(dir, newName + Path.GetFileName(f)[old.Length..]), true);
    }

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
