using System;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;

namespace MNRLauncher.Core;

/// <summary>
/// Decrypts a PS3 SELF (Secure Executable File) format into a plain ELF executable.
///
/// Mirrors RPCS3's unself implementation (rpcs3/Crypto/unself.cpp):
/// 1. Parses the SCE header and extended header (types, offsets, sizes).
/// 2. Handles NPDRM layer: decrypts metadata info using klicensee (free license) or key revision's NPDRM key.
/// 3. Decrypts section headers and data keys with AES-128-CTR using the metadata key/IV.
/// 4. For each program section: AES-128-CTR decrypt with per-section key/IV, zlib inflate if compressed.
/// 5. Writes ELF header, program headers, section headers, and decrypted sections to output.
///
/// Keys are the public NPDRM keys shipped with RPCS3. Debug SELFs (key revision high bit set) are not supported.
/// </summary>
public static class SelfDecrypter
{
    // NPDRM keys per key revision: (revision, encryption root key, initialization vector)
    // Public keys from RPCS3's Crypto/key_vault.h
    static readonly (int Rev, string Erk, string Riv)[] NpdrmKeys =
    {
        (0x0001, "F9EDD0301F770FABBA8863D9897F0FEA6551B09431F61312654E28F43533EA6B", "A551CCB4A42C37A734A2B4F9657D5540"),
        (0x0002, "8E737230C80E66AD0162EDDD32F1F774EE5E4E187449F19079437A508FCF9C86", "7AAECC60AD12AED90C348D8C11D2BED5"),
        (0x0003, "1B715B0C3E8DC4C1A5772EBA9C5D34F7CCFE5B82025D453F3167566497239664", "E31E206FBB8AEA27FAB0D9A2FFB6B62F"),
        (0x0004, "BB4DBF66B744A33934172D9F8379A7A5EA74CB0F559BB95D0E7AECE91702B706", "ADF7B207A15AC601110E61DDFC210AF6"),
        (0x0006, "8B4C52849765D2B5FA3D5628AFB17644D52B9FFEE235B4C0DB72A62867EAA020", "05719DF1B1D0306C03910ADDCE4AF887"),
        (0x0007, "3946DFAA141718C7BE339A0D6C26301C76B568AEBC5CD52652F2E2E0297437C3", "E4897BE553AE025CDCBF2B15D1C9234E"),
        (0x0009, "0786F4B0CA5937F515BDCE188F569B2EF3109A4DA0780A7AA07BD89C3350810A", "04AD3C2F122A3B35E804850CAD142C6D"),
        (0x000A, "03C21AD78FBB6A3D425E9AAB1298F9FD70E29FD4E6E3A3C151205DA50C413DE4", "0A99D4D4F8301A88052D714AD2FB565E"),
        (0x000C, "357EBBEA265FAEC271182D571C6CD2F62CFA04D325588F213DB6B2E0ED166D92", "D26E6DD2B74CD78E866E742E5571B84F"),
        (0x000D, "337A51416105B56E40D7CAF1B954CDAF4E7645F28379904F35F27E81CA7B6957", "8405C88E042280DBD794EC7E22B74002"),
        (0x000F, "135C098CBE6A3E037EBE9F2BB9B30218DDE8D68217346F9AD33203352FBB3291", "4070C898C2EAAD1634A288AA547A35A8"),
        (0x0010, "4B3CD10F6A6AA7D99F9B3A660C35ADE08EF01C2C336B9E46D1BB5678B4261A61", "C0F2AB86E6E0457552DB50D7219371C5"),
        (0x0013, "265C93CF48562EC5D18773BEB7689B8AD10C5EB6D21421455DEBC4FB128CBF46", "8DEA5FF959682A9B98B688CEA1EF4A1D"),
        (0x0016, "7910340483E419E55F0D33E4EA5410EEEC3AF47814667ECA2AA9D75602B14D4B", "4AD981431B98DFD39B6388EDAD742A8E"),
        (0x0019, "FBDA75963FE690CFF35B7AA7B408CF631744EDEF5F7931A04D58FD6A921FFDB3", "F72C1D80FFDA2E3BF085F4133E6D2805"),
        (0x001C, "8103EA9DB790578219C4CEDF0592B43064A7D98B601B6C7BC45108C4047AA80F", "246F4B8328BE6A2D394EDE20479247C5"),
    };
    // NP_KLIC_FREE: the klicensee for free-license SELFs (decrypted with NP_KLIC_KEY)
    static readonly byte[] KlicFree = Convert.FromHexString("72F990788F9CFF745725F08E4C128387");
    // NP_KLIC_KEY: master key used to decrypt the free-license klicensee
    static readonly byte[] KlicKey = Convert.FromHexString("F2FBCA7A75B04EDC1390638CCDFDD1EE");

    static ulong U64(byte[] b, long o) { ulong v = 0; for (int i = 0; i < 8; i++) v = v << 8 | b[o + i]; return v; }
    static uint U32(byte[] b, long o) => (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]);
    static ushort U16(byte[] b, long o) => (ushort)(b[o] << 8 | b[o + 1]);

    // AES-128-CTR decryption: encrypts a counter block and XORs with plaintext
    static void Ctr(byte[] key, byte[] ivIn, byte[] buf, int off, int len)
    {
        using var aes = Aes.Create(); aes.Key = key;
        var ctr = (byte[])ivIn.Clone(); var ks = new byte[16];
        for (int i = 0; i < len; i += 16)
        {
            aes.EncryptEcb(ctr, ks, PaddingMode.None);
            for (int k = 0; k < 16 && i + k < len; k++) buf[off + i + k] ^= ks[k];
            for (int k = 15; k >= 0 && ++ctr[k] == 0; k--) { }
        }
    }

    /// <summary>
    /// Decrypts a SELF binary into a plain ELF executable.
    /// Validates SELF magic, type, and key revision. Handles NPDRM layer, section decryption, and zlib decompression.
    /// </summary>
    /// <param name="f">The encrypted SELF data (e.g., EBOOT.BIN content).</param>
    /// <returns>The decrypted ELF bytes, or throws InvalidDataException on failure.</returns>
    public static byte[] Decrypt(byte[] f)
    {
        // Validate SELF magic ("SCE\0") and extended header
        if (f.Length < 0x90 || U32(f, 0) != 0x53434500) throw new InvalidDataException("Not an encrypted PS3 executable (SELF).");
        if (U16(f, 10) != 1) throw new InvalidDataException("This SCE file isn't a SELF.");
        int keyRev = U16(f, 8); uint metaOff = U32(f, 12); ulong hdrLen = U64(f, 16);
        if ((keyRev & 0x8000) != 0) throw new InvalidDataException("Debug SELFs aren't supported.");
        ulong elfOff = U64(f, 0x30), phdrOff = U64(f, 0x38), shdrOff = U64(f, 0x40), suppOff = U64(f, 0x58), suppSize = U64(f, 0x60);

        // --- NPDRM layer: decrypt metadata info ---
        // metadata info is at metaOff + 0x20 (40 bytes)
        var info = new byte[0x40]; Array.Copy(f, (long)metaOff + 0x20, info, 0, 0x40);
        // Walk supplemental headers to find NPD (license type 3 = free)
        for (ulong p = suppOff; p < suppOff + suppSize && p + 16 <= (ulong)f.Length;)
        {
            uint type = U32(f, (long)p), size = U32(f, (long)p + 4); ulong next = U64(f, (long)p + 8);
            if (type == 3) // NPD header found
            {
                long npd = (long)p + 16;
                if (U32(f, npd) != 0x4E504400) throw new InvalidDataException("Bad NPDRM header.");
                uint license = U32(f, npd + 8);
                if (license != 3) throw new InvalidDataException("This EBOOT needs a license file (it isn't the free-license update).");
                // Decrypt klicensee: NP_KLIC_KEY -> AES-ECB -> klicensee
                using var a = Aes.Create(); a.Key = KlicKey;
                var k = a.DecryptEcb(KlicFree, PaddingMode.None);
                // Decrypt metadata info with klicensee: AES-128-CBC, zero IV
                using var b = Aes.Create(); b.Key = k;
                info = b.DecryptCbc(info, new byte[16], PaddingMode.None);
            }
            if (next == 0 || size == 0) break;
            p += size;
        }
        // Find the NPDRM key for this key revision and decrypt metadata info
        byte[]? meta = null;
        foreach (var (rev, erk, riv) in NpdrmKeys)
        {
            if (rev != keyRev) continue;
            using var a = Aes.Create(); a.Key = Convert.FromHexString(erk);
            var t = a.DecryptCbc(info, Convert.FromHexString(riv), PaddingMode.None);
            // Validate: padding bytes at offsets 0x10 and 0x30 should be zero
            if (t[0x10] == 0 && t[0x30] == 0) { meta = t; break; }
        }
        if (meta == null) throw new InvalidDataException($"No key for this executable (key revision 0x{keyRev:X4}).");
        byte[] mkey = meta[0..16], miv = meta[0x20..0x30];

        // --- Decrypt metadata header and section headers with AES-128-CTR ---
        long mhOff = (long)metaOff + 0x20 + 0x40; int mhLen = (int)((long)hdrLen - mhOff);
        var mh = new byte[mhLen]; Array.Copy(f, mhOff, mh, 0, mhLen);
        Ctr(mkey, miv, mh, 0, mhLen);
        int secCount = (int)U32(mh, 0x0C), keyCount = (int)U32(mh, 0x10);
        int keysAt = 0x20 + secCount * 0x30;

        // --- Write ELF output: header, then sections, then headers ---
        long e = (long)elfOff;
        ulong phoff = U64(f, e + 0x20), shoff = U64(f, e + 0x28);
        int phnum = U16(f, e + 0x38), shnum = U16(f, e + 0x3C);
        var outp = new MemoryStream();
        outp.Write(f, (int)e, 0x40); // Write ELF header as-is
        // Process each section
        for (int s = 0; s < secCount; s++)
        {
            int h = 0x20 + s * 0x30;
            ulong dOff = U64(mh, h), dSize = U64(mh, h + 8);
            uint type = U32(mh, h + 0x10), prog = U32(mh, h + 0x14), enc = U32(mh, h + 0x20), ki = U32(mh, h + 0x24), ii = U32(mh, h + 0x28), comp = U32(mh, h + 0x2C);
            if (type != 2) continue; // Only process program sections (type 2)
            var data = new byte[dSize]; Array.Copy(f, (long)dOff, data, 0, (long)dSize);
            // AES-128-CTR decrypt with per-section key/IV
            if (enc == 3)
            {
                if (ki >= keyCount || ii >= keyCount) throw new InvalidDataException("Bad section key.");
                Ctr(mh[(keysAt + (int)ki * 16)..(keysAt + (int)ki * 16 + 16)], mh[(keysAt + (int)ii * 16)..(keysAt + (int)ii * 16 + 16)], data, 0, data.Length);
            }
            // Zlib decompress if marked compressed
            long ph = (long)phdrOff + prog * 0x38;
            ulong pOff = U64(f, ph + 8), pFilesz = U64(f, ph + 0x20);
            if (comp == 2)
            {
                using var z = new ZLibStream(new MemoryStream(data), CompressionMode.Decompress);
                var d = new byte[pFilesz]; int got = 0, n;
                while (got < d.Length && (n = z.Read(d, got, d.Length - got)) > 0) got += n;
                data = d[..got];
            }
            outp.Position = (long)pOff; outp.Write(data, 0, data.Length);
        }
        // Write section headers and program headers as-is (from the original SELF)
        if (shoff != 0 && shnum > 0) { outp.Position = (long)shoff; outp.Write(f, (int)shdrOff, shnum * 0x40); }
        if (phnum > 0) { outp.Position = (long)phoff; outp.Write(f, (int)phdrOff, phnum * 0x38); }
        return outp.ToArray();
    }
}
