using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace MNRLauncher.Core;

/// <summary>
/// Complete installation pipeline for the public edition of ModNation Racers.
///
/// The PC version is built entirely from the player's own PS3 disc and update package.
/// The pipeline has 5 stages, each skipped if its output already exists (resumable):
///
/// Stage 1 - Disc: Extracts a decrypted ISO or game folder into extracted\. Only the US disc (BCUS98167) works.
/// Stage 2 - Update: Installs the 1.11 update .pkg into hdd0\game\BCUS98167\.
/// Stage 3 - Executable: Decrypts the update's EBOOT.BIN into re\bin_v111\EBOOT.elf via SelfDecrypter.
/// Stage 4 - Tools: Downloads llvm-mingw (C compiler) and Python (build script runtime) into tools\.
/// Stage 5 - Build: Runs re\recomp\build.py to translate EBOOT.elf to C and compile mnr.exe (~10 min).
///
/// Each stage reports progress via the Report delegate and validates its inputs (checksums, content IDs).
/// </summary>
public static class GameSetup
{
    const string EbootElfSha = "1da79f0955289357ec36a5684085009b549f1da8bb390e304d27dd7796d4c3b2";
    const string LlvmUrl = "https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-x86_64.zip";
    const string LlvmSha = "317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a";
    public const string LlvmDirName = "llvm-mingw-20261006-ucrt-x86_64";   // (build.py's compiler path)
    const string PythonUrl = "https://www.python.org/ftp/python/3.14.0/python-3.14.0-embed-amd64.zip";
    const string PythonSha = "8d4d3590c10449d78aa4375f534e6d5f3027d67fdc362dd1a882279db6f90fdf";
    public const long NeedBytes = 20L << 30;

    public static string ToolsDir => Path.Combine(Paths.Root, "tools");
    static string LlvmDir => Path.Combine(ToolsDir, LlvmDirName);
    static string PythonExe => Path.Combine(ToolsDir, "python", "python.exe");
    static string RecompDir => Path.Combine(Paths.ReDir, "recomp");

    public delegate void Report(string text, double fraction);   // fraction < 0: no bar (unknown length)

    public static bool DiscDone => File.Exists(Path.Combine(Paths.GameData, "USRDIR", "EBOOT.BIN")) && File.Exists(Path.Combine(Paths.GameData, "PARAM.SFO"));
    public static bool UpdateDone => File.Exists(Paths.Eboot) && File.Exists(Path.Combine(Paths.InstalledUsrdir, "EBOOT.BIN"));
    public static bool BuildDone => File.Exists(Paths.GameExe);
    public static bool RuntimePresent => File.Exists(Path.Combine(RecompDir, "build.py")) && File.Exists(Path.Combine(RecompDir, "ppu2c.py"));

    /// <summary>Voice chat's codec: already taken out of a system update (firmware\libcelpdec.prx), or a system update file
    /// where the game looks for one (the disc's PS3_UPDATE, or firmware\PS3UPDAT.PUP).</summary>
    public static bool VoiceReady =>
        File.Exists(Path.Combine(Paths.Root, "firmware", "libcelpdec.prx")) || File.Exists(Path.Combine(Paths.Root, "extracted", "PS3_UPDATE", "PS3UPDAT.PUP"))
        || File.Exists(Path.Combine(Paths.Root, "firmware", "PS3UPDAT.PUP"));
    public static bool Complete => DiscDone && UpdateDone && BuildDone;

    /// <summary>The player's PS3 system update file, for voice chat: copied to firmware\PS3UPDAT.PUP (voice_celp.c looks there).</summary>
    public static void UseSystemUpdate(string pup)
    {
        using (var f = File.OpenRead(pup)) { var h = new byte[5]; f.ReadExactly(h); if (h[0] != 'S' || h[1] != 'C' || h[2] != 'E' || h[3] != 'U' || h[4] != 'F') throw new InvalidDataException("That isn't a PS3 system update file (PS3UPDAT.PUP)."); }
        var dir = Path.Combine(Paths.Root, "firmware"); Directory.CreateDirectory(dir);
        File.Copy(pup, Path.Combine(dir, "PS3UPDAT.PUP"), true);
    }

    /// <summary>What an earlier, interrupted setup already put in place (counted against the space needed).</summary>
    public static long BytesAlreadyThere()
    {
        long n = 0;
        foreach (var d in new[] { Path.Combine(Paths.Root, "extracted"), Paths.Hdd0, ToolsDir })
            try { if (Directory.Exists(d)) foreach (var f in Directory.EnumerateFiles(d, "*", SearchOption.AllDirectories)) n += new FileInfo(f).Length; } catch (Exception) { }
        return n;
    }

    /// <summary>The PC runtime (re\recomp's source, the function list and the play profile) from the newest release.</summary>
    public static async Task FetchRuntime(Report report, CancellationToken ct)
    {
        var a = Updater.LoadAccount();
        report("Asking GitHub for the PC runtime…", -1);
        var m = await Updater.FetchManifest(a, ct);
        var p = await Task.Run(() => Updater.Compare(Paths.Root, m), ct);
        p = p with { LauncherUpdate = false };   // (the launcher updates itself from the Updates page)
        if (p.Packs.Count > 0) await Updater.Apply(Paths.Root, a, p, (d, t, text) => report(text, t > 0 ? (double)d / t : -1), ct);
        if (!RuntimePresent) throw new FileNotFoundException("The release has no PC runtime (re\\recomp).");
    }

    /// <summary>The title id in a PARAM.SFO (TITLE_ID), or null.</summary>
    static string? TitleId(string sfoPath) => File.Exists(sfoPath) ? PkgInstaller.SfoString(File.ReadAllBytes(sfoPath), "TITLE_ID") : null;

    // ---- 1. disc
    /// <summary>Copies PS3_GAME and the disc metadata from the disc image or folder into extracted\.</summary>
    public static async Task InstallDisc(string source, Report report, CancellationToken ct)
    {
        if (DiscDone) { report("Disc files: already copied.", 1); return; }
        var dest = Path.Combine(Paths.Root, "extracted");
        Directory.CreateDirectory(dest);
        if (File.Exists(source))
        {
            report("Reading the disc image…", -1);
            await Task.Run(() =>
            {
                using var iso = new IsoReader(source);
                var sfo = iso.Entries.Find(e => e.Path.Equals("PS3_GAME/PARAM.SFO", StringComparison.OrdinalIgnoreCase))
                          ?? throw new InvalidDataException("This disc image has no PS3_GAME/PARAM.SFO: is it a PS3 game?");
                var id = PkgInstaller.SfoString(iso.Read(sfo), "TITLE_ID");
                if (id != "BCUS98167") throw new InvalidDataException($"This is {id ?? "an unknown disc"}, not the US ModNation Racers disc (BCUS98167): the PC version is made from the US game and its 1.11 update.");
                iso.Extract(dest, (d, t) => report($"Copying the disc ({d / 1e9:0.0} / {t / 1e9:0.0} GB)", (double)d / t), ct);
            }, ct);
        }
        else if (Directory.Exists(source))
        {
            var root = Directory.Exists(Path.Combine(source, "PS3_GAME")) ? source
                     : Path.GetFileName(source).Equals("PS3_GAME", StringComparison.OrdinalIgnoreCase) ? Path.GetDirectoryName(source)! : source;
            if (!Directory.Exists(Path.Combine(root, "PS3_GAME", "USRDIR"))) throw new InvalidDataException("That folder has no PS3_GAME\\USRDIR: pick the game folder from your disc dump (the one containing PS3_GAME).");
            var id = TitleId(Path.Combine(root, "PS3_GAME", "PARAM.SFO"));
            if (id != "BCUS98167") throw new InvalidDataException($"This is {id ?? "an unknown game"}, not the US ModNation Racers (BCUS98167).");
            var eb = Path.Combine(root, "PS3_GAME", "USRDIR", "EBOOT.BIN");
            using (var f = File.OpenRead(eb)) { var h = new byte[3]; f.ReadExactly(h); if (h[0] != 'S' || h[1] != 'C' || h[2] != 'E') throw new InvalidDataException("The disc's EBOOT.BIN looks encrypted or damaged: use a decrypted dump."); }
            await Task.Run(() =>
            {
                var files = new System.Collections.Generic.List<(string From, string Rel, long Size)>();
                foreach (var f in Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories))
                {
                    var rel = Path.GetRelativePath(root, f).Replace('\\', '/');
                    if (IsoReader.Wanted(rel)) files.Add((f, rel, new FileInfo(f).Length));
                }
                long total = 0, done = 0; foreach (var x in files) total += x.Size;
                foreach (var (from, rel, size) in files)
                {
                    ct.ThrowIfCancellationRequested();
                    var to = Path.Combine(dest, rel.Replace('/', Path.DirectorySeparatorChar));
                    Directory.CreateDirectory(Path.GetDirectoryName(to)!);
                    File.Copy(from, to, true); done += size;
                    report($"Copying the disc ({done / 1e9:0.0} / {total / 1e9:0.0} GB)", total > 0 ? (double)done / total : 1);
                }
            }, ct);
        }
        else throw new FileNotFoundException("Pick a disc image (.iso) or a game folder.", source);
        if (!DiscDone) throw new InvalidDataException("The disc copy is incomplete (no PS3_GAME\\USRDIR\\EBOOT.BIN).");
        report("Disc files copied.", 1);
    }

    // ---- 2 + 3. update and executable
    /// <summary>Installs the 1.11 update package and writes the decrypted EBOOT.elf, verifying its hash.</summary>
    public static async Task InstallUpdate(string pkg, Report report, CancellationToken ct)
    {
        if (UpdateDone) { report("1.11 update: already installed.", 1); return; }
        if (!File.Exists(pkg)) throw new FileNotFoundException("Pick your ModNation Racers 1.11 update package (.pkg).", pkg);
        var id = PkgInstaller.ContentId(pkg);
        if (!id.StartsWith("UP9000-BCUS98167_00-MODNATIONKART002")) throw new InvalidDataException($"That package is {id}, not the ModNation Racers 1.11 update (UP9000-BCUS98167_00-MODNATIONKART002).");
        report("Installing the 1.11 update…", -1);
        await Task.Run(() => PkgInstaller.Install(pkg, Paths.Hdd0), ct);
        report("Decrypting the game's executable…", -1);
        var elf = await Task.Run(() => SelfDecrypter.Decrypt(File.ReadAllBytes(Path.Combine(Paths.InstalledUsrdir, "EBOOT.BIN"))), ct);
        if (Convert.ToHexString(SHA256.HashData(elf)).ToLowerInvariant() != EbootElfSha)
            throw new InvalidDataException("The update's executable isn't the 1.11 one this PC version is made for.");
        Directory.CreateDirectory(Path.GetDirectoryName(Paths.Eboot)!);
        await File.WriteAllBytesAsync(Paths.Eboot, elf, ct);
        report("1.11 update installed.", 1);
    }

    // ---- 4. tools
    /// <summary>Downloads the compiler and Python if they are missing.</summary>
    public static async Task EnsureTools(Report report, CancellationToken ct)
    {
        Directory.CreateDirectory(ToolsDir);
        if (!File.Exists(Path.Combine(LlvmDir, "bin", "clang.exe")))
            await FetchZip(LlvmUrl, LlvmSha, ToolsDir, "the C compiler (llvm-mingw, 191 MB)", report, ct);
        if (!File.Exists(PythonExe))
            await FetchZip(PythonUrl, PythonSha, Path.Combine(ToolsDir, "python"), "Python (12 MB)", report, ct);
        report("Tools ready.", 1);
    }

    static async Task FetchZip(string url, string sha, string extractTo, string what, Report report, CancellationToken ct)
    {
        var zip = Path.Combine(ToolsDir, Path.GetFileName(new Uri(url).AbsolutePath));
        if (!(File.Exists(zip) && await Sha(zip, ct) == sha))
        {
            await Download(url, zip, what, report, ct);
            if (await Sha(zip, ct) != sha) { File.Delete(zip); throw new InvalidDataException($"The download of {what} didn't match its known checksum. Try again."); }
        }
        report($"Unpacking {what}…", -1);
        await Task.Run(() => ZipFile.ExtractToDirectory(zip, extractTo, true), ct);
        File.Delete(zip);
    }

    // ---- 5. build
    /// <summary>Runs build.py to recompile the game (incremental, with PGO when the profile is present).</summary>
    public static async Task Build(Report report, Action<string> log, CancellationToken ct)
    {
        if (!RuntimePresent) throw new FileNotFoundException("The game's PC runtime (re\\recomp) is missing: check for updates first.");
        var psi = new ProcessStartInfo(PythonExe) { WorkingDirectory = RecompDir, UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var a in new[] { "-I", "build.py", "--game", "v111", "--regen", "--lto", "--pgo-use", Path.Combine("..", "mnr_race.profdata") }) psi.ArgumentList.Add(a);
        if (!File.Exists(Path.Combine(Paths.ReDir, "mnr_race.profdata"))) { psi.ArgumentList.Remove("--pgo-use"); psi.ArgumentList.Remove(Path.Combine("..", "mnr_race.profdata")); }
        using var p = Process.Start(psi)!;
        var tail = new System.Collections.Generic.Queue<string>();
        void Line(string? l)
        {
            if (l == null) return;
            lock (tail) { tail.Enqueue(l); while (tail.Count > 30) tail.Dequeue(); }
            var m = Regex.Match(l, @"^\[compile\] (\d+)/(\d+)");
            if (m.Success) report($"Building the game: compiling ({m.Groups[1].Value} of {m.Groups[2].Value})", 0.15 + 0.6 * int.Parse(m.Groups[1].Value) / int.Parse(m.Groups[2].Value));
            else if (l.StartsWith("[stage] translating the game")) report("Building the game: translating its code (a few minutes)", 0.02);
            else if (l.StartsWith("[stage] translating the SPU")) report("Building the game: translating its SPU programs", 0.12);
            else if (l.StartsWith("[stage] linking")) report("Building the game: linking and optimising (several minutes)", 0.8);
            if (l.StartsWith("[stage]") || l.StartsWith("built ")) log(l);
        }
        p.OutputDataReceived += (_, e) => Line(e.Data); p.ErrorDataReceived += (_, e) => Line(e.Data);
        p.BeginOutputReadLine(); p.BeginErrorReadLine();
        using (ct.Register(() => { try { p.Kill(true); } catch (Exception) { } }))
            await p.WaitForExitAsync(CancellationToken.None);
        ct.ThrowIfCancellationRequested();
        if (p.ExitCode != 0 || !BuildDone)
        {
            string t; lock (tail) t = string.Join("\n", tail);
            throw new InvalidOperationException("The build failed:\n" + t);
        }
        report("The game is built.", 1);
    }

    // ---- helpers
    static async Task<string> Sha(string path, CancellationToken ct)
    {
        await using var f = File.OpenRead(path);
        return Convert.ToHexString(await SHA256.HashDataAsync(f, ct)).ToLowerInvariant();
    }

    static async Task Download(string url, string to, string what, Report report, CancellationToken ct)
    {
        using var h = new HttpClient { Timeout = TimeSpan.FromHours(2) };
        h.DefaultRequestHeaders.UserAgent.ParseAdd("MNRLauncher");
        using var r = await h.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, ct);
        r.EnsureSuccessStatusCode();
        long total = r.Content.Headers.ContentLength ?? -1, got = 0;
        var tmp = to + ".part";
        await using (var src = await r.Content.ReadAsStreamAsync(ct))
        await using (var dst = File.Create(tmp))
        {
            var buf = new byte[1 << 20]; int n;
            while ((n = await src.ReadAsync(buf, ct)) > 0)
            {
                await dst.WriteAsync(buf.AsMemory(0, n), ct); got += n;
                report($"Downloading {what} ({got / 1e6:0} / {(total > 0 ? $"{total / 1e6:0}" : "?")} MB)", total > 0 ? (double)got / total : -1);
            }
        }
        File.Move(tmp, to, true);
    }

    /// <summary>Free space on the drive of the MNR folder, in bytes.</summary>
    public static long FreeBytes() { try { return new DriveInfo(Path.GetPathRoot(Path.GetFullPath(Paths.Root))!).AvailableFreeSpace; } catch (Exception) { return long.MaxValue; } }
}
