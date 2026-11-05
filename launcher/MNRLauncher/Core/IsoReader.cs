using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading;

namespace MNRLauncher.Core;

/// <summary>
/// Custom ISO 9660 parser for decrypted PS3 disc images.
///
/// Reads 2048-byte sectors starting from sector 16 (volume descriptors).
/// Prefers Joliet supplementary descriptors for full Unicode filenames; falls back to ISO 9660.
/// Supports multi-extent files (files >4 GB split across multiple directory records).
///
/// Extract() validates the disc by checking EBOOT.BIN starts with "SCE" (rejects raw Redump dumps),
/// then copies PS3_GAME/, PS3_UPDATE/, and PS3_DISC.SFB to the destination folder.
/// </summary>
public sealed class IsoReader : IDisposable
{
    /// <summary>A file or directory entry from the ISO's directory records.</summary>
    public record Entry(string Path, bool IsDir, List<(long Lba, long Size)> Extents)
    {
        /// <summary>Total size across all extents (multi-extent files). O(n) - computed each time.</summary>
        public long Size { get { long s = 0; foreach (var e in Extents) s += e.Size; return s; } }
    }

    readonly FileStream f;
    public List<Entry> Entries { get; } = new();

    /// <summary>Scans sectors 16-63 for ISO 9660 and Joliet volume descriptors, then walks the directory tree.</summary>
    public IsoReader(string path)
    {
        // 1 MB buffer for sector reads
        f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
        byte[]? pvd = null, joliet = null;
        // ISO 9660 volume descriptors start at sector 16: type 1 = primary, type 2 = supplementary (Joliet)
        for (int s = 16; s < 64; s++)
        {
            var d = Sector(s);
            // Volume descriptor type must be "CD001" at offset 1
            if (d[1] != 'C' || d[2] != 'D' || d[3] != '0' || d[4] != '0' || d[5] != '1') break;
            // Type 1: Primary Volume Descriptor (ISO 9660 root path)
            if (d[0] == 1 && pvd == null) pvd = d;
            // Type 2: Supplementary Volume Descriptor (Joliet) - check magic \x25\x2F@/C/E
            else if (d[0] == 2 && d[88] == 0x25 && d[89] == 0x2F && (d[90] == 0x40 || d[90] == 0x43 || d[90] == 0x45)) joliet = d;
            // Type 255 = boot sector: end of descriptors
            else if (d[0] == 255) break;
        }
        // Joliet preferred for full filenames; ISO 9660 is 8.3 format
        var vd = joliet ?? pvd ?? throw new InvalidDataException("This isn't a disc image (no ISO 9660 volume).");
        // Start directory tree walk from root extent (stored in volume descriptor)
        Walk(BitConverter.ToUInt32(vd, 156 + 2), BitConverter.ToUInt32(vd, 156 + 10), "", joliet != null, 0);
    }

    // Read a single 2048-byte sector at the given logical block address
    byte[] Sector(long lba) { var b = new byte[2048]; f.Position = lba * 2048; f.ReadExactly(b); return b; }

    // Recursively walk a directory's data, parsing ISO 9660 directory records
    void Walk(long lba, long size, string dir, bool joliet, int depth)
    {
        if (depth > 32) return;
        var data = new byte[size]; f.Position = lba * 2048; f.ReadExactly(data);
        Entry? last = null;
        for (int p = 0; p < data.Length;)
        {
            int len = data[p];
            // Zero-length record = padding to next sector boundary
            if (len == 0) { p = (p / 2048 + 1) * 2048; continue; }
            long ext = BitConverter.ToUInt32(data, p + 2), sz = BitConverter.ToUInt32(data, p + 10);
            int flags = data[p + 25], nlen = data[p + 32];
            string name;
            // . and .. directory entries have name length 1 with content 0 or 1
            if (nlen == 1 && (data[p + 33] == 0 || data[p + 33] == 1)) name = "";
            else name = joliet ? Encoding.BigEndianUnicode.GetString(data, p + 33, nlen & ~1) : Encoding.ASCII.GetString(data, p + 33, nlen);
            // Strip ISO 9660 version suffixes (e.g., ";1")
            int semi = name.IndexOf(';'); if (semi >= 0) name = name[..semi];
            // ISO 9660 trailing dot convention
            if (!joliet && name.EndsWith('.')) name = name[..^1];
            p += len;
            if (name.Length == 0) continue;
            var path = dir.Length == 0 ? name : dir + "/" + name;
            bool isDir = (flags & 2) != 0;
            // Multi-extent file: same path, not a directory = additional extent of a large file
            if (last != null && last.Path == path && !isDir) { last.Extents.Add((ext, sz)); continue; }
            var e = new Entry(path, isDir, new List<(long, long)> { (ext, sz) });
            Entries.Add(e); last = e;
            if (isDir) Walk(ext, sz, path, joliet, depth + 1);
        }
    }

    /// <summary>Reads all extents of a file entry into a single byte array.</summary>
    public byte[] Read(Entry e)
    {
        var b = new byte[e.Size]; long o = 0;
        foreach (var (lba, sz) in e.Extents) { f.Position = lba * 2048; f.ReadExactly(b, (int)o, (int)sz); o += sz; }
        return b;
    }

    /// <summary>
    /// Extracts PS3_GAME/, PS3_UPDATE/, and PS3_DISC.SFB from the disc image to the destination folder.
    /// Validates the disc by checking EBOOT.BIN starts with "SCE" (rejects raw/encrypted Redump dumps).
    /// Reports progress via (bytesDone, totalBytes) callback.
    /// </summary>
    public void Extract(string dest, Action<long, long>? progress, CancellationToken ct)
    {
        // Find EBOOT.BIN to validate disc type
        var eboot = Entries.Find(e => e.Path.Equals("PS3_GAME/USRDIR/EBOOT.BIN", StringComparison.OrdinalIgnoreCase))
                    ?? throw new InvalidDataException("No PS3_GAME/USRDIR/EBOOT.BIN on this disc image: is it a PS3 game?");
        f.Position = eboot.Extents[0].Lba * 2048; var head = new byte[4]; f.ReadExactly(head);
        if (head[0] != 'S' || head[1] != 'C' || head[2] != 'E')
            throw new InvalidDataException("This disc image is still encrypted (a raw Redump dump). Decrypt it first (for example with PS3Dec and the disc's key), or use a decrypted ISO or a game folder.");
        long total = 0, done = 0;
        var files = Entries.FindAll(e => !e.IsDir && Wanted(e.Path));
        foreach (var e in files) total += e.Size;
        var buf = new byte[1 << 20];
        foreach (var e in files)
        {
            ct.ThrowIfCancellationRequested();
            var target = System.IO.Path.GetFullPath(System.IO.Path.Combine(dest, e.Path.Replace('/', System.IO.Path.DirectorySeparatorChar)));
            // Path traversal check: prevent .. from escaping dest
            if (!target.StartsWith(System.IO.Path.GetFullPath(dest) + System.IO.Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) continue;
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(target)!);
            using var o = new FileStream(target, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20);
            foreach (var (lba, sz) in e.Extents)
            {
                f.Position = lba * 2048;
                for (long left = sz; left > 0;)
                {
                    int n = (int)Math.Min(buf.Length, left); f.ReadExactly(buf, 0, n); o.Write(buf, 0, n);
                    left -= n; done += n; progress?.Invoke(done, total);
                }
            }
        }
    }

    /// <summary>True for the disc paths the launcher extracts (PS3_GAME, PS3_UPDATE and PS3_DISC.SFB).</summary>
    public static bool Wanted(string path) =>
        path.StartsWith("PS3_GAME/", StringComparison.OrdinalIgnoreCase) || path.StartsWith("PS3_UPDATE/", StringComparison.OrdinalIgnoreCase)
        || path.Equals("PS3_DISC.SFB", StringComparison.OrdinalIgnoreCase);

    /// <summary>Closes the disc image.</summary>
    public void Dispose() => f.Dispose();
}
