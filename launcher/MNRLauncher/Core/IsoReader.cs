using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading;

namespace MNRLauncher.Core;

/// <summary>Reads a decrypted PS3 disc image (.iso, ISO 9660: 2048-byte sectors, volume descriptors from sector 16; the
/// Joliet supplementary descriptor, when there is one, gives the full file names). Directory records: {u8 length,
/// u8 ext attr, u32le+be extent, u32le+be size, 7-byte date, u8 flags (2 = directory, 0x80 = more extents follow),
/// .., u8 name length, name}. Files over 4 GB come as several records with the same name (multi-extent).
/// An encrypted image (a raw Redump dump) has the same readable file system but scrambled file data: Extract checks
/// the disc's EBOOT.BIN starts with "SCE" and refuses otherwise.</summary>
public sealed class IsoReader : IDisposable
{
    public record Entry(string Path, bool IsDir, List<(long Lba, long Size)> Extents)
    {
        public long Size { get { long s = 0; foreach (var e in Extents) s += e.Size; return s; } }
    }

    readonly FileStream f;
    public List<Entry> Entries { get; } = new();

    public IsoReader(string path)
    {
        f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
        byte[]? pvd = null, joliet = null;
        for (int s = 16; s < 64; s++)
        {
            var d = Sector(s);
            if (d[1] != 'C' || d[2] != 'D' || d[3] != '0' || d[4] != '0' || d[5] != '1') break;
            if (d[0] == 1 && pvd == null) pvd = d;
            else if (d[0] == 2 && d[88] == 0x25 && d[89] == 0x2F && (d[90] == 0x40 || d[90] == 0x43 || d[90] == 0x45)) joliet = d;
            else if (d[0] == 255) break;
        }
        var vd = joliet ?? pvd ?? throw new InvalidDataException("This isn't a disc image (no ISO 9660 volume).");
        Walk(BitConverter.ToUInt32(vd, 156 + 2), BitConverter.ToUInt32(vd, 156 + 10), "", joliet != null, 0);
    }

    byte[] Sector(long lba) { var b = new byte[2048]; f.Position = lba * 2048; f.ReadExactly(b); return b; }

    void Walk(long lba, long size, string dir, bool joliet, int depth)
    {
        if (depth > 32) return;
        var data = new byte[size]; f.Position = lba * 2048; f.ReadExactly(data);
        Entry? last = null;
        for (int p = 0; p < data.Length;)
        {
            int len = data[p];
            if (len == 0) { p = (p / 2048 + 1) * 2048; continue; }   // records don't cross sectors: the rest of this one is padding
            long ext = BitConverter.ToUInt32(data, p + 2), sz = BitConverter.ToUInt32(data, p + 10);
            int flags = data[p + 25], nlen = data[p + 32];
            string name;
            if (nlen == 1 && (data[p + 33] == 0 || data[p + 33] == 1)) name = "";   // . and ..
            else name = joliet ? Encoding.BigEndianUnicode.GetString(data, p + 33, nlen & ~1) : Encoding.ASCII.GetString(data, p + 33, nlen);
            int semi = name.IndexOf(';'); if (semi >= 0) name = name[..semi];
            if (!joliet && name.EndsWith('.')) name = name[..^1];
            p += len;
            if (name.Length == 0) continue;
            var path = dir.Length == 0 ? name : dir + "/" + name;
            bool isDir = (flags & 2) != 0;
            if (last != null && last.Path == path && !isDir) { last.Extents.Add((ext, sz)); continue; }   // next extent of a big file
            var e = new Entry(path, isDir, new List<(long, long)> { (ext, sz) });
            Entries.Add(e); last = e;
            if (isDir) Walk(ext, sz, path, joliet, depth + 1);
        }
    }

    public byte[] Read(Entry e)
    {
        var b = new byte[e.Size]; long o = 0;
        foreach (var (lba, sz) in e.Extents) { f.Position = lba * 2048; f.ReadExactly(b, (int)o, (int)sz); o += sz; }
        return b;
    }

    /// <summary>Copies the disc's PS3_GAME, PS3_UPDATE and PS3_DISC.SFB into dest. Progress: (bytes done, total).</summary>
    public void Extract(string dest, Action<long, long>? progress, CancellationToken ct)
    {
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

    public static bool Wanted(string path) =>
        path.StartsWith("PS3_GAME/", StringComparison.OrdinalIgnoreCase) || path.StartsWith("PS3_UPDATE/", StringComparison.OrdinalIgnoreCase)
        || path.Equals("PS3_DISC.SFB", StringComparison.OrdinalIgnoreCase);

    public void Dispose() => f.Dispose();
}
