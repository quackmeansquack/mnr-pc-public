using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace MNRLauncher.Core;

/// <summary>Installs a PS3 package (.pkg, as the PS3's own package installer and RPCS3 do) of the player's own DLC into the
/// game's hdd0 folder: the files go to hdd0\game\&lt;title id&gt;\, the title id being characters 7..15 of the package's
/// content id (UP9000-BCUS98167_00-...). DLC comes as retail (finalized) packages; mods, retail or debug, go through
/// ExtractMod into the launcher's mods folder instead.
///
/// Format: big-endian header (magic 7F 'PKG', u16 revision 0x8000 = retail, u16 type 1 = PS3, ..., u32 item count at
/// 0x14, u64 total size 0x18, u64 data offset 0x20, u64 data size 0x28, content id 0x30, IV 0x70). The data area is
/// AES-128-CTR encrypted with the PS3 package key, the counter starting at the IV for the first byte of the area. It
/// starts with the item table, 32 bytes per item {u32 name offset, u32 name size, u64 data offset, u64 data size,
/// u32 flags, u32 pad} (offsets relative to the data area); flags' low byte 4 = a directory.</summary>
public static class PkgInstaller
{
    static readonly byte[] PkgKeyPs3 = Convert.FromHexString("2E7B71D7C9C9A14EA3221F188828B8F8");

    /// <summary>Outcome of a package install: content id, title id, file count and whether it was licensed.</summary>
    public record Result(string ContentId, string TitleId, int Files, bool Licensed = false);

    // NPDRM keys (as RPCS3's Crypto/key_vault.h): .rap -> RIF key, and the EDAT key for encrypted block keys
    // NP_RAP_KEY: master key used to decrypt the first 16 bytes of a .rap file to get the RIF key
    static readonly byte[] RapKey = Convert.FromHexString("869F7745C13FD890CCF29188E3CC3EDF");
    // NP_RAP_PBOX: permutation box used in RIF key derivation
    static readonly byte[] RapPbox = { 0x0C, 0x03, 0x06, 0x04, 0x01, 0x0B, 0x0F, 0x08, 0x02, 0x07, 0x00, 0x05, 0x0A, 0x0E, 0x0D, 0x09 };
    // NP_RAP_E1: first round encryption constants for RIF key derivation
    static readonly byte[] RapE1 = Convert.FromHexString("A93E1FD67C55A329B75FDDA62A95C7A5");
    // NP_RAP_E2: second round encryption constants for RIF key derivation
    static readonly byte[] RapE2 = Convert.FromHexString("67D45DA3296D006A4E7C537BF5538C74");
    // NP_EDAT_KEY0: EDAT decryption key for version != 4
    static readonly byte[] EdatKey0 = Convert.FromHexString("BE959CA8308DEFA2E5E180C63712A9AE");
    // NP_EDAT_KEY1: EDAT decryption key for version == 4
    static readonly byte[] EdatKey1 = Convert.FromHexString("4CA9C14B01C95309969BEC68AA0BC081");
    /// <summary>ModNation Racers' own license key (klicensee, at 0x137DB5C in the 1.11 executable, which it hands to
    /// sceNpDrmIsAvailable): it decrypts the free DLC packs' licenses (NPD license type 3), no .rap needed.</summary>
    static readonly byte[] GameKlic = Convert.FromHexString("C0A3B3641C2AD1EF23153A48A3E12FE7");

    /// <summary>
    /// Derives the RIF key from a .rap file (the account's purchase license).
    /// Mirrors RPCS3's rap_to_rif: AES-CBC decrypt, then 5 rounds of permutation/XOR/subtraction to recover the klicensee.
    /// </summary>
    /// <param name="rap">The .rap file content (at least 16 bytes).</param>
    /// <returns>The 16-byte RIF key (klicensee) for the account's license.</returns>
    public static byte[] RifKeyFromRap(byte[] rap)
    {
        using var aes = Aes.Create(); aes.Key = RapKey;
        var key = aes.DecryptCbc(rap[..16], new byte[16], PaddingMode.None);
        // 5 rounds of permutation, XOR, and subtraction to derive the final key
        for (int round = 0; round < 5; round++)
        {
            for (int i = 0; i < 16; i++) { int p = RapPbox[i]; key[p] ^= RapE1[p]; }
            for (int i = 15; i >= 1; i--) { int p = RapPbox[i], pp = RapPbox[i - 1]; key[p] ^= key[pp]; }
            int o = 0;
            for (int i = 0; i < 16; i++)
            {
                int p = RapPbox[i];
                byte kc = (byte)(key[p] - o), ec2 = RapE2[p];
                if (o != 1 || kc != 0xFF) { o = kc < ec2 ? 1 : 0; key[p] = (byte)(kc - ec2); }
                else if (kc == 0xFF) key[p] = (byte)(kc - ec2);
                else key[p] = kc;
            }
        }
        return key;
    }

    /// <summary>
    /// Decrypts an EDAT (NPD header 0x80, EDAT header {flags, block size, u64 file size}, block hashes from 0x100)
    /// with a license key, as RPCS3's unedat (uncompressed files only; the block hashes aren't checked).
    /// </summary>
    /// <param name="f">The EDAT file content.</param>
    /// <param name="rifKey">The RIF key (klicensee) derived from the account's .rap or the game's free license.</param>
    /// <returns>The decrypted content (XML license data), or null if the EDAT is compressed, has unknown layout, or the key is wrong.</returns>
    public static byte[]? DecryptEdat(byte[] f, byte[] rifKey)
    {
        if (f.Length < 0x100 || U32(f, 0) != 0x4E504400) return null;
        int version = (int)U32(f, 4);
        uint flags = U32(f, 0x80), blockSize = U32(f, 0x84); ulong fileSize = U64(f, 0x88);
        // Only handle uncompressed, non-SDAT EDAT files
        if ((flags & 0x01) != 0 || (flags & 0x01000000) != 0 || blockSize == 0) return null;
        var digest = f[0x40..0x50]; var devHash = f[0x60..0x70];
        uint blocks = (uint)((fileSize + blockSize - 1) / blockSize);
        var outp = new byte[fileSize];
        using var aes = Aes.Create(); aes.Key = rifKey;
        using var edat = Aes.Create(); edat.Key = version == 4 ? EdatKey1 : EdatKey0;
        for (uint b = 0; b < blocks; b++)
        {
            long off; ulong len = blockSize;
            // Calculate data offset based on layout flags (encrypted block key vs plaintext)
            if ((flags & 0x20) != 0) off = 0x100 + (long)b * (0x20 + blockSize) + 0x20;
            else off = 0x100 + (long)b * blockSize + blocks * 0x10L;
            if (b == blocks - 1 && fileSize % blockSize != 0) len = fileSize % blockSize;
            ulong padded = (len + 15) & ~15UL;
            if (off + (long)padded > f.Length) return null;
            // Build per-block key: devHash (for version > 1) + block number as big-endian u32
            var bkey = new byte[16];
            if (version > 1) Array.Copy(devHash, bkey, 12);
            bkey[12] = (byte)(b >> 24); bkey[13] = (byte)(b >> 16); bkey[14] = (byte)(b >> 8); bkey[15] = (byte)b;
            var keyResult = aes.EncryptEcb(bkey, PaddingMode.None);
            // Decrypt the block key if it's encrypted (flag 0x08)
            if ((flags & 0x08) != 0) keyResult = edat.DecryptCbc(keyResult, new byte[16], PaddingMode.None);
            var data = f[(int)off..(int)(off + (long)padded)];
            byte[] plain;
            // Decrypt the block data with the per-block key (AES-128-CBC)
            if ((flags & 0x02) != 0) plain = data;   // no encryption
            else { using var k = Aes.Create(); k.Key = keyResult; plain = k.DecryptCbc(data, version > 1 ? digest : new byte[16], PaddingMode.None); }
            Array.Copy(plain, 0, outp, (long)b * blockSize, (long)len);
        }
        // A wrong key gives noise: the game's licenses are XML (maybe after a UTF-8 byte order mark)
        return outp.Length > 0 && (outp[0] == (byte)'<' || (outp.Length > 3 && outp[0] == 0xEF && outp[3] == (byte)'<')) ? outp : null;
    }

    static uint U32(byte[] b, long o) => (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]);
    static ulong U64(byte[] b, long o) => (ulong)U32(b, o) << 32 | U32(b, o + 4);

    /// <summary>
    /// Decrypts <paramref name="len"/> bytes of the data area starting at <paramref name="rel"/> (relative to the data area start).
    /// Uses AES-128-CTR with the PS3 package key and the IV from the PKG header.
    /// </summary>
    static byte[] Decrypt(byte[] pkg, ulong dataOffset, byte[] iv, ulong rel, ulong len)
    {
        var outp = new byte[len];
        using var aes = Aes.Create(); aes.Key = PkgKeyPs3;
        var ctr = new byte[16]; var ks = new byte[16];
        ulong block = rel / 16; int skip = (int)(rel % 16);
        for (ulong done = 0; done < len; block++)
        {
            // Counter = IV + block (128-bit big-endian addition)
            Array.Copy(iv, ctr, 16);
            ulong carry = block;
            for (int i = 15; i >= 0 && carry != 0; i--) { ulong s = ctr[i] + (carry & 0xFF); ctr[i] = (byte)s; carry = (carry >> 8) + (s >> 8); }
            aes.EncryptEcb(ctr, ks, PaddingMode.None);
            for (int i = skip; i < 16 && done < len; i++, done++) outp[done] = (byte)(pkg[(long)(dataOffset + rel + done)] ^ ks[i]);
            skip = 0;
        }
        return outp;
    }

    /// <summary>
    /// Decrypts a debug package (revision 0x0000, as homebrew packagers make them):
    /// the key stream is the first 16 bytes of SHA-1 over {digest[0..8], digest[0..8], digest[8..16], digest[8..16],
    /// 0x18 zero bytes, u64 block number}, the digest being the header's 16 bytes at 0x60 (RPCS3's unpkg).
    /// </summary>
    static byte[] DecryptDebug(byte[] pkg, ulong dataOffset, byte[] digest, ulong rel, ulong len)
    {
        var outp = new byte[len];
        var input = new byte[64]; var ks = new byte[20];
        Array.Copy(digest, 0, input, 0, 8); Array.Copy(digest, 0, input, 8, 8);
        Array.Copy(digest, 8, input, 16, 8); Array.Copy(digest, 8, input, 24, 8);
        ulong block = rel / 16; int skip = (int)(rel % 16);
        for (ulong done = 0; done < len; block++)
        {
            for (int i = 0; i < 8; i++) input[56 + i] = (byte)(block >> (56 - 8 * i));
            SHA1.HashData(input, ks);
            for (int i = skip; i < 16 && done < len; i++, done++) outp[done] = (byte)(pkg[(long)(dataOffset + rel + done)] ^ ks[i]);
            skip = 0;
        }
        return outp;
    }

    /// <summary>An entry in the PKG's item table.</summary>
    record Item(string Name, bool IsDir, ulong Offset, ulong Size);

    /// <summary>
    /// Validates the PKG header, reads the item table, and returns a data reader function.
    /// Returns: (content id, list of items, decrypt function for the data area).
    /// </summary>
    static (string ContentId, List<Item> Items, Func<ulong, ulong, byte[]> Read) Open(byte[] pkg)
    {
        // Validate PKG magic ("PKG\7F"), type (PS3 = 1), and bounds
        if (pkg.Length < 0xC0 || U32(pkg, 0) != 0x7F504B47) throw new InvalidDataException("Not a PS3 package (no PKG header).");
        if ((pkg[6] << 8 | pkg[7]) != 1) throw new InvalidDataException("This isn't a PS3 package (PSP or other).");
        uint items = U32(pkg, 0x14);
        ulong total = U64(pkg, 0x18), dataOff = U64(pkg, 0x20), dataSize = U64(pkg, 0x28);
        if ((ulong)pkg.Length < total || dataOff + dataSize > (ulong)pkg.Length) throw new InvalidDataException("The package is incomplete (cut short).");
        string contentId = Encoding.ASCII.GetString(pkg, 0x30, 0x30).TrimEnd('\0');
        if (contentId.Length < 16) throw new InvalidDataException("The package has no content id.");
        Func<ulong, ulong, byte[]> read;
        // Retail package (bit 7 of byte 4 set): uses IV from header
        if ((pkg[4] & 0x80) != 0) { var iv = pkg[0x70..0x80]; read = (rel, len) => Decrypt(pkg, dataOff, iv, rel, len); }
        // Debug package: uses SHA-1 based key stream
        else { var digest = pkg[0x60..0x70]; read = (rel, len) => DecryptDebug(pkg, dataOff, digest, rel, len); }
        if ((ulong)items * 32 > dataSize) throw new InvalidDataException("The package's file table is damaged.");
        var table = read(0, (ulong)items * 32);
        var list = new List<Item>();
        for (uint i = 0; i < items; i++)
        {
            long e = i * 32;
            uint nameOff = U32(table, e), nameSize = U32(table, e + 4), flags = U32(table, e + 24);
            ulong off = U64(table, e + 8), size = U64(table, e + 16);
            if (nameOff + nameSize > dataSize || off + size > dataSize) throw new InvalidDataException("The package's file table is damaged.");
            // Decode item name and check if it's a directory (flags low byte = 4)
            list.Add(new Item(Encoding.UTF8.GetString(read(nameOff, nameSize)).TrimEnd('\0'), (flags & 0xFF) == 4, off, size));
        }
        return (contentId, list, read);
    }

    /// <summary>
    /// Checks whether a package is DLC (it carries DRM-*.EDAT licenses) rather than a mod.
    /// Returns true if any non-directory item has a .EDAT extension.
    /// </summary>
    public static bool IsDlc(string pkgPath)
    {
        var (_, items, _) = Open(File.ReadAllBytes(pkgPath));
        return items.Exists(i => !i.IsDir && i.Name.EndsWith(".EDAT", StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>Outcome of a mod package extraction: folder, name, file count and skipped entries.</summary>
    public record ModResult(string Folder, string Name, int Files, List<string> Skipped);

    /// <summary>
    /// Extracts a PS3 package of game files (a mod, debug or retail) into a launcher mod (ModManager format).
    /// Game files go to &lt;mod&gt;\files\; PARAM.SFO's TITLE and APP_VER set the mod name and version.
    /// Program code (EBOOT.BIN, .self, .sprx) and encrypted EDAT/SDAT files are skipped and listed.
    /// </summary>
    public static ModResult ExtractMod(string pkgPath, string modsDir)
    {
        var (contentId, items, read) = Open(File.ReadAllBytes(pkgPath));
        string titleId = contentId.Substring(7, 9);
        string? title = null, version = null;
        var skipped = new List<string>();
        // Validate title ID matches ModNation Racers
        if (!(titleId is "BCUS98167" or "BCES00701" or "BCES00764" or "BCAS20112" or "BCJS30040")) skipped.Add($"(made for {titleId}, not ModNation Racers)");
        // Parse PARAM.SFO to extract TITLE and APP_VER
        var sfo = items.Find(i => !i.IsDir && i.Name.Equals("PARAM.SFO", StringComparison.OrdinalIgnoreCase));
        if (sfo != null) { var s = read(sfo.Offset, sfo.Size); title = SfoString(s, "TITLE"); version = SfoString(s, "APP_VER"); }
        var name = string.IsNullOrWhiteSpace(title) ? contentId : title!;
        Directory.CreateDirectory(modsDir);
        var dest = ModManager.Unique(Path.Combine(modsDir, ModManager.SafeName(name)));
        var filesDir = Path.Combine(dest, "files");
        int files = 0;
        try
        {
            foreach (var it in items)
            {
                if (it.IsDir) continue;
                var n = it.Name.Replace('\\', '/');
                // Filter: only files inside USRDIR are game data
                int u = n.StartsWith("USRDIR/", StringComparison.OrdinalIgnoreCase) ? 0 : n.IndexOf("/USRDIR/", StringComparison.OrdinalIgnoreCase) + 1;
                if (u == 0 && !n.StartsWith("USRDIR/", StringComparison.OrdinalIgnoreCase)) continue;   // PARAM.SFO, ICON0.PNG, ...: not game data
                var rel = n.Substring(u + 7);
                var ext = Path.GetExtension(rel).ToUpperInvariant();
                // Skip program code and encrypted files
                if (rel.Equals("EBOOT.BIN", StringComparison.OrdinalIgnoreCase) || ext is ".SELF" or ".SPRX" or ".ELF") { skipped.Add($"{rel} (program code)"); continue; }
                if (ext is ".EDAT" or ".SDAT") { skipped.Add($"{rel} (encrypted)"); continue; }
                var target = Path.GetFullPath(Path.Combine(filesDir, rel.Replace('/', Path.DirectorySeparatorChar)));
                if (!target.StartsWith(filesDir + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException($"The package names a file outside its folder: {it.Name}");
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                File.WriteAllBytes(target, read(it.Offset, it.Size));
                files++;
            }
            if (files == 0) throw new InvalidDataException("The package has no game files (nothing for USRDIR) to use as a mod.");
            // Write mod.json with metadata
            var info = new ModInfo { Name = name, Version = string.IsNullOrWhiteSpace(version) ? "1.0" : version!.Trim(), Description = $"From {Path.GetFileName(pkgPath)} ({contentId})" };
            File.WriteAllText(Path.Combine(dest, "mod.json"), System.Text.Json.JsonSerializer.Serialize(info, new System.Text.Json.JsonSerializerOptions { WriteIndented = true }));
        }
        catch { try { Directory.Delete(dest, true); } catch (Exception) { } throw; }
        return new ModResult(dest, name, files, skipped);
    }

    /// <summary>
    /// Extracts a text value from a PARAM.SFO file (little-endian: magic "\0PSF", key/data tables, 16-byte entries).
    /// Returns the UTF-8 string for the given key, or null if not found.
    /// </summary>
    internal static string? SfoString(byte[] s, string key)
    {
        if (s.Length < 0x14 || s[0] != 0 || s[1] != (byte)'P' || s[2] != (byte)'S' || s[3] != (byte)'F') return null;
        int keys = BitConverter.ToInt32(s, 8), data = BitConverter.ToInt32(s, 12), n = BitConverter.ToInt32(s, 16);
        for (int i = 0; i < n && 0x14 + i * 16 + 16 <= s.Length; i++)
        {
            int e = 0x14 + i * 16;
            int ko = keys + BitConverter.ToUInt16(s, e), len = BitConverter.ToInt32(s, e + 4), dof = data + BitConverter.ToInt32(s, e + 12);
            int kend = Array.IndexOf(s, (byte)0, ko); if (kend < 0) return null;
            if (Encoding.ASCII.GetString(s, ko, kend - ko) != key) continue;
            if (dof < 0 || len < 0 || dof + len > s.Length) return null;
            return Encoding.UTF8.GetString(s, dof, len).TrimEnd('\0');
        }
        return null;
    }

    /// <summary>Reads a package's content id without installing it.</summary>
    public static string ContentId(string pkgPath)
    {
        using var f = File.OpenRead(pkgPath);
        var h = new byte[0x60]; if (f.Read(h, 0, h.Length) != h.Length) throw new InvalidDataException("Not a PS3 package.");
        if (U32(h, 0) != 0x7F504B47) throw new InvalidDataException("Not a PS3 package (no PKG header).");
        return Encoding.ASCII.GetString(h, 0x30, 0x30).TrimEnd('\0');
    }

    /// <summary>
    /// Installs a retail PS3 package (.pkg) into the game's hdd0/game/&lt;title id&gt;\ folder, as the PS3's own installer does.
    /// Decrypts the data area (AES-128-CTR for retail, SHA-1 based for debug), processes the item table, and writes files.
    /// EDAT license files are decrypted with the account's .rap (if present) or the game's free license key.
    /// </summary>
    public static Result Install(string pkgPath, string hdd0, bool listOnly = false, bool force = false)
    {
        var pkg = File.ReadAllBytes(pkgPath);
        // Validate PKG header, type, and bounds
        if (pkg.Length < 0xC0 || U32(pkg, 0) != 0x7F504B47) throw new InvalidDataException("Not a PS3 package (no PKG header).");
        if ((pkg[4] & 0x80) == 0 && !force) throw new InvalidDataException("This is a debug package (a mod): import it on the Mods page.");
        if ((pkg[6] << 8 | pkg[7]) != 1) throw new InvalidDataException("This isn't a PS3 package (PSP or other).");
        uint items = U32(pkg, 0x14);
        ulong total = U64(pkg, 0x18), dataOff = U64(pkg, 0x20), dataSize = U64(pkg, 0x28);
        if ((ulong)pkg.Length < total || dataOff + dataSize > (ulong)pkg.Length) throw new InvalidDataException("The package is incomplete (cut short).");
        string contentId = Encoding.ASCII.GetString(pkg, 0x30, 0x30).TrimEnd('\0');
        if (contentId.Length < 16) throw new InvalidDataException("The package has no content id.");
        string titleId = contentId.Substring(7, 9);
        // Map all regional title IDs to the US title ID (DLC licenses are region-agnostic)
        if (titleId is "BCES00701" or "BCES00764" or "BCAS20112" or "BCJS30040") titleId = "BCUS98167";
        Func<ulong, ulong, byte[]> read;
        // Retail package: uses IV from header; Debug package: uses SHA-1 based key stream
        if ((pkg[4] & 0x80) != 0) { var iv = new byte[16]; Array.Copy(pkg, 0x70, iv, 0, 16); read = (rel, len) => Decrypt(pkg, dataOff, iv, rel, len); }
        else { var digest = pkg[0x60..0x70]; read = (rel, len) => DecryptDebug(pkg, dataOff, digest, rel, len); }
        var table = read(0, (ulong)items * 32);
        var root = Path.GetFullPath(Path.Combine(hdd0, "game", titleId));
        // Load the account's RIF key from a .rap file next to the package (if present)
        var rapPath = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(pkgPath))!, contentId + ".rap");
        byte[]? rifKey = File.Exists(rapPath) && new FileInfo(rapPath).Length >= 16 ? RifKeyFromRap(File.ReadAllBytes(rapPath)) : null;
        bool licensed = false;
        int files = 0;
        var written = new List<string>();
        for (uint i = 0; i < items; i++)
        {
            long e = i * 32;
            uint nameOff = U32(table, e), nameSize = U32(table, e + 4), flags = U32(table, e + 24);
            ulong off = U64(table, e + 8), size = U64(table, e + 16);
            if (nameOff + nameSize > dataSize || off + size > dataSize) throw new InvalidDataException("The package's file table is damaged.");
            var name = Encoding.UTF8.GetString(read(nameOff, nameSize)).TrimEnd('\0').Replace('/', Path.DirectorySeparatorChar);
            var dest = Path.GetFullPath(Path.Combine(root, name));
            if (!dest.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException($"The package names a file outside its folder: {name}");
            if (listOnly) { Console.WriteLine($"{((flags & 0xFF) == 4 ? "dir " : size.ToString().PadLeft(8))}  {dest}{(File.Exists(dest) ? "  (exists)" : "")}"); continue; }
            if ((flags & 0xFF) == 4) { Directory.CreateDirectory(dest); continue; }
            var content = read(off, size);
            // Determine the decryption key for EDAT license files: free pack uses GameKlic, bought pack uses rifKey
            byte[]? key = content.Length >= 0x10 && (U32(content, 8) & 3) == 3 ? GameKlic : rifKey;
            // Decrypt EDAT license files (DRM-*.EDAT) with the appropriate key
            if (dest.EndsWith(".EDAT", StringComparison.OrdinalIgnoreCase) && key != null && DecryptEdat(content, key) is byte[] plain)
            {
                content = plain; licensed = true;
                // Handle license number conflicts: if the destination already exists with different content, find the first free DRM-N.EDAT slot
                var m = System.Text.RegularExpressions.Regex.Match(Path.GetFileName(dest), @"^DRM-(\d+)\.EDAT$", System.Text.RegularExpressions.RegexOptions.IgnoreCase);
                if (m.Success && File.Exists(dest) && !File.ReadAllBytes(dest).AsSpan().SequenceEqual(content))
                {
                    var dir = Path.GetDirectoryName(dest)!; string? free = null;
                    for (int k = 1; k <= 89 && free == null; k++)
                    {
                        var cand = Path.Combine(dir, $"DRM-{k}.EDAT");
                        if (!File.Exists(cand)) free = cand;
                        else if (File.ReadAllBytes(cand).AsSpan().SequenceEqual(content)) { free = ""; }   // already installed under another number
                    }
                    if (free == "") continue;
                    if (free == null) throw new IOException("All 89 DLC license slots are taken.");
                    dest = free;
                }
            }
            else if (File.Exists(dest)) continue;
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            File.WriteAllBytes(dest, content);
            written.Add(dest); files++;
        }
        return new Result(contentId, titleId, files, licensed);
    }
}
