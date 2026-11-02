using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Xml.Linq;

namespace MNRLauncher.Core;

/// <summary>Installs a creation exactly as the game does when it downloads one from the server it plays on.
///
/// The game keeps creations by "published state" in USRDIR\1\DATA: 0 Local (made here, ids 5000-9999), 2 Upload (made
/// here and shared), 3 Download (someone else's), 5 Remote (fetched on the fly in lobbies); 2, 3 and 5 need server ids
/// (10000 and up). Every published creation is checked against the server it plays on, and anything that doesn't add up
/// is deleted. A creation's .CMD (CreationMetaDataV7, big-endian, its struct from file offset 0x10) holds:
///   0x50 CRC-64 of the .SAV/.TRK      0x58 CRC-64 of the metadata (CmdCheck)     0x60 creation id   0x64 creator id
///   0x68 parent creation id   0x6C parent's creator id   0x70 original creator id   0x78 version   0x7C/0x80 counts of
///   the DRM and keyword hash lists   0x84 type (0 kart, 1 track, 2 character)   0x88 published state
///   0x90/0x94 the two lists, 0x98..0xB0 seven strings (name, description, creator, parent name, parent's creator,
///   original creator, region), each as an offset from its own position.
/// Downloading, the game takes data.bin from the server (the creation and its .CMD as the creator uploaded them),
/// rewrites the ids, version, state and creator names from the server's player_creations/&lt;id&gt;.xml record and
/// re-signs it (Rebuild reproduces the game's file byte for byte), into Data\Download\&lt;Type&gt;\&lt;id&gt;_&lt;name&gt;.
///
/// Creations from the MNR Revival site keep their Revival id. A server with the import (our PLGarage, mnrpc/caps)
/// first copies the creation from Revival under that id, owned by an account named after its creator; Revival serves
/// its own creations directly.</summary>
public static class CreationDownload
{
    public const string RevivalAddress = "45.32.179.33";
    static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(30) };
    static string DataDir => Path.Combine(Paths.CreationsDir, "1", "DATA");

    // ---------------------------------------------------------------- the game's checks
    static readonly ulong[] Table = MakeTable();
    static ulong[] MakeTable()
    {
        var t = new ulong[256];
        for (uint i = 0; i < 256; i++) { ulong c = i; for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? (c >> 1) ^ 0x95AC9329AC4BC9B5UL : c >> 1; t[i] = c; }
        return t;   // the reflected Jones polynomial: t[1] = 0x7AD870C830358979, as in the game's table
    }
    /// <summary>The game's CRC-64 (seed all ones, no final xor), continuing from crc.</summary>
    public static ulong Crc64(ReadOnlySpan<byte> d, ulong crc = ulong.MaxValue)
    {
        foreach (var b in d) crc = Table[(byte)(crc ^ b)] ^ (crc >> 8);
        return crc;
    }
    static uint U32(byte[] c, int o) => (uint)(c[o] << 24 | c[o + 1] << 16 | c[o + 2] << 8 | c[o + 3]);
    static void Put32(byte[] c, int o, uint v) { c[o] = (byte)(v >> 24); c[o + 1] = (byte)(v >> 16); c[o + 2] = (byte)(v >> 8); c[o + 3] = (byte)v; }
    static void Put64(byte[] c, int o, ulong v) { Put32(c, o, (uint)(v >> 32)); Put32(c, o + 4, (uint)v); }
    static int? Rel(byte[] c, int o) { var v = U32(c, o); return v == 0 ? null : o + (int)v; }
    static byte[] CString(byte[] c, int? at) { if (at == null) return Array.Empty<byte>(); int e = Array.IndexOf(c, (byte)0, at.Value); return c.AsSpan(at.Value, e - at.Value).ToArray(); }

    /// <summary>The metadata check at 0x58 (the game's f_00651010): CRC-64 chained over bytes 0x50..0x8F with 0x58..0x5F
    /// zeroed, the two hash lists, then the seven strings (without their zeros).</summary>
    public static ulong CmdCheck(byte[] c)
    {
        var blk = c.AsSpan(0x50, 0x40).ToArray(); Array.Clear(blk, 8, 8);
        var crc = Crc64(blk);
        foreach (var (ptr, cnt) in new[] { (0x90, 0x7C), (0x94, 0x80) })
        {
            var p = Rel(c, ptr); var n = (int)U32(c, cnt);
            if (p != null && n > 0) crc = Crc64(c.AsSpan(p.Value, 4 * n), crc);
        }
        for (int ptr = 0x98; ptr < 0xB4; ptr += 4) crc = Crc64(CString(c, Rel(c, ptr)), crc);
        return crc;
    }
    /// <summary>Both of the game's checks hold: the data CRC (0x50) and the metadata CRC (0x58).</summary>
    public static bool Valid(byte[] cmd, byte[] data) =>
        cmd.Length >= 0xB8 && U32(cmd, 0) == 0xBFDB1ACDu && Crc64(data) == ((ulong)U32(cmd, 0x50) << 32 | U32(cmd, 0x54)) && CmdCheck(cmd) == ((ulong)U32(cmd, 0x58) << 32 | U32(cmd, 0x5C));

    /// <summary>The .CMD as the game writes it for a download (state 3) or one's own restored creation (state 2): ids,
    /// version and creator names from the server's record, the creation's name, description, region and hash lists kept,
    /// lists then strings packed after the header (an empty list takes a zero byte), padded to 4, re-signed.</summary>
    public static byte[] Rebuild(byte[] cmd, IReadOnlyDictionary<string, string> rec, uint state)
    {
        uint Num(string k) => rec.TryGetValue(k, out var v) && long.TryParse(v, out var n) ? (uint)n : 0;
        string Str(string k) => rec.TryGetValue(k, out var v) ? v : "";
        var head = cmd.AsSpan(0, 0xB8).ToArray();
        Put32(head, 0x60, Num("id")); Put32(head, 0x64, Num("player_id")); Put32(head, 0x68, Num("parent_creation_id"));
        Put32(head, 0x6C, Num("parent_player_id")); Put32(head, 0x70, Num("original_player_id")); Put32(head, 0x78, Num("version"));
        Put32(head, 0x88, state);
        var lists = new[] { (0x90, 0x7C), (0x94, 0x80) }.Select(x => { var p = Rel(cmd, x.Item1); var n = (int)U32(cmd, x.Item2); return p != null && n > 0 ? cmd.AsSpan(p.Value, 4 * n).ToArray() : Array.Empty<byte>(); }).ToArray();
        var strs = Enumerable.Range(0, 7).Select(i => CString(cmd, Rel(cmd, 0x98 + 4 * i))).ToArray();
        var latin = Encoding.Latin1;
        strs[2] = latin.GetBytes(Str("username")); strs[3] = latin.GetBytes(Str("parent_creation_name"));
        strs[4] = latin.GetBytes(Str("parent_player_username")); strs[5] = latin.GetBytes(Str("original_player_username"));
        var body = new List<byte>();
        for (int i = 0; i < 2; i++) { Put32(head, 0x90 + 4 * i, (uint)(0xB8 + body.Count - (0x90 + 4 * i))); body.AddRange(lists[i].Length > 0 ? lists[i] : new byte[1]); }
        for (int i = 0; i < 7; i++) { Put32(head, 0x98 + 4 * i, (uint)(0xB8 + body.Count - (0x98 + 4 * i))); body.AddRange(strs[i]); body.Add(0); }
        while ((0xB8 + body.Count) % 4 != 0) body.Add(0);
        var outp = head.Concat(body).ToArray();
        Put32(outp, 0x04, (uint)(outp.Length - 16)); Put32(outp, 0x08, (uint)(outp.Length - 16));
        Put64(outp, 0x58, CmdCheck(outp));
        return outp;
    }

    // ---------------------------------------------------------------- the server
    public static string ServerBase(Settings s) =>
        $"http://{(s.ServerAddress.Trim().Length > 0 ? s.ServerAddress.Trim() : RevivalAddress)}:{(s.ServerPort > 0 ? s.ServerPort : 10050)}";
    public static bool OnRevival(Settings s) => s.ServerAddress.Trim() == RevivalAddress;

    public static async Task<bool> CanImport(string server)
    {
        try { return (await Http.GetStringAsync($"{server}/mnrpc/caps")).StartsWith("mnrpc import"); } catch (Exception) { return false; }
    }
    /// <summary>The server's record of a creation (the attributes of its player_creations/&lt;id&gt;.xml), or null.</summary>
    public static async Task<Dictionary<string, string>?> Record(string server, int id)
    {
        var x = XDocument.Parse(await Http.GetStringAsync($"{server}/player_creations/{id}.xml"));
        if ((string?)x.Root?.Element("status")?.Element("id") != "0") return null;
        var pc = x.Root!.Element("response")?.Element("player_creation");
        return pc?.Attributes().ToDictionary(a => a.Name.LocalName, a => a.Value);
    }
    /// <summary>Asks a server with the import to copy a Revival creation. True when it has it afterwards.</summary>
    public static async Task<bool> Import(string server, int id)
    {
        using var r = await Http.PostAsync($"{server}/mnrpc/import?id={id}", null);
        return r.IsSuccessStatusCode;
    }

    // ---------------------------------------------------------------- the files
    static readonly (string type, string folder, string ext)[] Kinds = { ("KART", "Vehicle", ".SAV"), ("CHARACTER", "Character", ".SAV"), ("TRACK", "Track", ".TRK") };
    static string FolderOf(string type) => Kinds.First(k => k.type == type).folder;
    public static string FileName(string name) { var n = Regex.Replace(name, "[^A-Za-z0-9]", "").ToUpperInvariant(); return n.Length > 0 ? n : "CREATION"; }   // (as the game names its downloads: 00007148_DDLOBOTOMYCANYON)
    /// <summary>The creation's own name (the .CMD's first string): the game names a download's files after it
    /// (its "Zurg" is 00007b5b_Zurg.sav, whatever the server lists it as).</summary>
    public static string NameInCmd(byte[] cmd) => cmd.Length >= 0xB8 ? Encoding.Latin1.GetString(CString(cmd, Rel(cmd, 0x98))) : "";

    /// <summary>A downloaded creation's files (any name) by id, in Data\Download.</summary>
    public static string? Installed(int id)
    {
        foreach (var k in Kinds)
        {
            var dir = Path.Combine(DataDir, "DOWNLOAD", k.folder);
            if (Directory.Exists(dir)) { var f = Directory.GetFiles(dir, $"{id:X8}_*.CMD").FirstOrDefault(); if (f != null) return f[..^4]; }
        }
        return null;
    }
    public static void Remove(int id)
    {
        for (string? stem; (stem = Installed(id)) != null; )
            foreach (var f in Directory.GetFiles(Path.GetDirectoryName(stem)!, Path.GetFileName(stem) + "*")) File.Delete(f);
    }

    /// <summary>Downloads creation id as the game would from the server the game plays on (asking it to import from
    /// Revival first when it can). Returns the installed files' stem.</summary>
    public static async Task<string> Install(Settings s, int id, Action<string>? status = null)
    {
        var server = ServerBase(s);
        if (!OnRevival(s) && await CanImport(server)) { status?.Invoke("Adding it to your server…"); await Import(server, id); }
        status?.Invoke("Downloading…");
        var rec = await Record(server, id) ?? throw new IOException(OnRevival(s) || await CanImport(server)
            ? "The server doesn't have this creation." : "The server you play on doesn't have this creation and can't fetch it from MNR Revival.");
        var type = rec.GetValueOrDefault("player_creation_type") ?? "";
        if (!Kinds.Any(k => k.type == type)) throw new IOException($"Not a character, kart or track ({type}).");
        var data = await Http.GetByteArrayAsync($"{server}/player_creations/{id}/data.bin");
        if (data.Length < 12 || data[0] != 'C' || data[1] != 'T' || data[2] != 'H') throw new IOException("The server sent something that isn't a creation.");
        int bodyLen = (int)U32(data, 4), cmdLen = (int)U32(data, 8);
        var body = data.AsSpan(12, bodyLen).ToArray(); var cmd = data.AsSpan(12 + bodyLen, cmdLen).ToArray();
        var newCmd = Rebuild(cmd, rec, 3);
        if (!Valid(newCmd, body)) throw new IOException("The creation's data doesn't match its own checksum; the game would delete it.");
        Remove(id);
        var dir = Path.Combine(DataDir, "DOWNLOAD", FolderOf(type));
        Directory.CreateDirectory(dir);
        var own = NameInCmd(newCmd);
        var stem = Path.Combine(dir, $"{id:X8}_{FileName(own.Length > 0 ? own : rec.GetValueOrDefault("name") ?? "")}");
        await File.WriteAllBytesAsync(stem + Kinds.First(k => k.type == type).ext, body);
        await File.WriteAllBytesAsync(stem + ".CMD", newCmd);
        try
        {
            await File.WriteAllBytesAsync(stem + ".PNG", await Http.GetByteArrayAsync($"{server}/player_creations/{id}/preview_image.png"));
            CreationInstaller.WriteSmallFrom(stem + ".PNG", stem + "_SMALL.PNG");
        }
        catch (Exception) { /* no picture: the menus show a blank one */ }
        return stem;
    }
}
