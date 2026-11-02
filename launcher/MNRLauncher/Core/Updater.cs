using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;

namespace MNRLauncher.Core;

/// <summary>
/// Installs and updates the whole build (game, native exe, launcher) from GitHub Releases of a private repository.
///
/// Each release carries manifest.json: the files of the install grouped into "packs" (zip assets of at most ~1.9 GB,
/// GitHub's limit is 2 GB), with every file's size and SHA-256, and the release each pack lives in (unchanged packs
/// stay in the release that first uploaded them). The launcher compares the manifest with the disk and downloads only
/// the packs holding missing or changed files, one at a time (temporary space: one pack). The launcher itself is the
/// "launcher" entry: a newer one is swapped in and restarted. Published by re\scripts\publish_release.py.
///
/// Access: a GitHub token with read access to the repository (fine-grained, "Contents: read-only"), kept with the
/// repository name in %APPDATA%\MNRLauncher\github.json, encrypted for the Windows user.
/// </summary>
public static class Updater
{
    public sealed class FileEntry
    {
        [JsonPropertyName("path")] public string Path { get; set; } = "";
        [JsonPropertyName("size")] public long Size { get; set; }
        [JsonPropertyName("sha256")] public string Sha256 { get; set; } = "";
    }
    public sealed class Pack
    {
        [JsonPropertyName("name")] public string Name { get; set; } = "";
        [JsonPropertyName("release")] public string Release { get; set; } = "";
        [JsonPropertyName("asset")] public string Asset { get; set; } = "";
        [JsonPropertyName("size")] public long Size { get; set; }
        [JsonPropertyName("sha256")] public string Sha256 { get; set; } = "";
        [JsonPropertyName("files")] public List<FileEntry> Files { get; set; } = new();
    }
    public sealed class LauncherEntry
    {
        [JsonPropertyName("release")] public string Release { get; set; } = "";
        [JsonPropertyName("asset")] public string Asset { get; set; } = "";
        [JsonPropertyName("size")] public long Size { get; set; }
        [JsonPropertyName("sha256")] public string Sha256 { get; set; } = "";
    }
    public sealed class Manifest
    {
        [JsonPropertyName("version")] public string Version { get; set; } = "";
        [JsonPropertyName("notes")] public string Notes { get; set; } = "";
        [JsonPropertyName("packs")] public List<Pack> Packs { get; set; } = new();
        [JsonPropertyName("launcher")] public LauncherEntry? Launcher { get; set; }
    }

    // ---------------------------------------------------------------- account (per Windows user)
    public sealed class Account
    {
        public string Repo { get; set; } = "";              // owner/name
        public string TokenProtected { get; set; } = "";    // DPAPI, base64
        [JsonIgnore] public string Token
        {
            get
            {
                if (TokenProtected.Length == 0) return "";
                try { return System.Text.Encoding.UTF8.GetString(ProtectedData.Unprotect(Convert.FromBase64String(TokenProtected), null, DataProtectionScope.CurrentUser)); }
                catch (Exception) { return ""; }
            }
            set => TokenProtected = string.IsNullOrEmpty(value) ? "" : Convert.ToBase64String(ProtectedData.Protect(System.Text.Encoding.UTF8.GetBytes(value), null, DataProtectionScope.CurrentUser));
        }
#if PUBLIC_EDITION
        public bool Ready => Repo.Contains('/');   // (a public repository needs no token; one is used while it is private)
#else
        public bool Ready => Repo.Contains('/') && TokenProtected.Length > 0;
#endif
    }
#if PUBLIC_EDITION
    const string AppDataName = "MNRLauncher Public";   // (its own: the private launcher on the same PC keeps its repository, token and folder)
#else
    const string AppDataName = "MNRLauncher";
#endif
    public static string AppDataDir => System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), AppDataName);
    static string AccountFile => System.IO.Path.Combine(AppDataDir, "github.json");
    /// <summary>(public edition) the repository its releases come from: the PC runtime's source and the launcher.</summary>
    public const string PublicRepo = "quackmeansquack/mnr-pc-public";
    public static Account LoadAccount()
    {
        Account a;
        try { a = File.Exists(AccountFile) ? JsonSerializer.Deserialize<Account>(File.ReadAllText(AccountFile)) ?? new Account() : new Account(); }
        catch (Exception) { a = new Account(); }
#if PUBLIC_EDITION
        a.Repo = PublicRepo;
#endif
        return a;
    }
    public static void SaveAccount(Account a)
    {
        Directory.CreateDirectory(AppDataDir);
        File.WriteAllText(AccountFile, JsonSerializer.Serialize(a, new JsonSerializerOptions { WriteIndented = true }));
    }

    // ---------------------------------------------------------------- GitHub
    static HttpClient Client(string token)
    {
        var h = new HttpClient(new HttpClientHandler { AllowAutoRedirect = true }) { Timeout = Timeout.InfiniteTimeSpan };
        h.DefaultRequestHeaders.UserAgent.ParseAdd("MNRLauncher/1.0");
        if (token.Length > 0) h.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", token);
        h.DefaultRequestHeaders.Add("X-GitHub-Api-Version", "2022-11-28");
        return h;
    }

    sealed class GhAsset { [JsonPropertyName("id")] public long Id { get; set; } [JsonPropertyName("name")] public string Name { get; set; } = ""; [JsonPropertyName("size")] public long Size { get; set; } }
    sealed class GhRelease { [JsonPropertyName("tag_name")] public string Tag { get; set; } = ""; [JsonPropertyName("assets")] public List<GhAsset> Assets { get; set; } = new(); }

    static async Task<GhRelease> Release(HttpClient h, string repo, string? tag, CancellationToken ct)
    {
        var url = tag == null ? $"https://api.github.com/repos/{repo}/releases/latest" : $"https://api.github.com/repos/{repo}/releases/tags/{Uri.EscapeDataString(tag)}";
        using var r = await h.GetAsync(url, ct);
        if (r.StatusCode == System.Net.HttpStatusCode.NotFound)
            throw new IOException(tag == null ? $"No release found in {repo} (or the token can't see that repository)." : $"Release {tag} is missing from {repo}.");
        if (r.StatusCode == System.Net.HttpStatusCode.Unauthorized) throw new IOException("GitHub refused the token (expired or mistyped?).");
        r.EnsureSuccessStatusCode();
        return JsonSerializer.Deserialize<GhRelease>(await r.Content.ReadAsStringAsync(ct)) ?? throw new IOException("GitHub sent an empty release.");
    }

    static async Task Download(HttpClient h, string repo, long assetId, string to, long size, Action<long>? progress, CancellationToken ct)
    {
        var req = new HttpRequestMessage(HttpMethod.Get, $"https://api.github.com/repos/{repo}/releases/assets/{assetId}");
        req.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/octet-stream"));
        using var r = await h.SendAsync(req, HttpCompletionOption.ResponseHeadersRead, ct);
        r.EnsureSuccessStatusCode();
        await using var src = await r.Content.ReadAsStreamAsync(ct);
        await using var dst = File.Create(to);
        var buf = new byte[1 << 20]; long got = 0; int n;
        while ((n = await src.ReadAsync(buf, ct)) > 0) { await dst.WriteAsync(buf.AsMemory(0, n), ct); got += n; progress?.Invoke(got); }
        if (size > 0 && got != size) throw new IOException($"Download incomplete ({got} of {size} bytes).");
    }

    public static async Task<Manifest> FetchManifest(Account a, CancellationToken ct = default)
    {
        using var h = Client(a.Token);
        var rel = await Release(h, a.Repo, null, ct);
        var asset = rel.Assets.FirstOrDefault(x => x.Name == "manifest.json") ?? throw new IOException($"Release {rel.Tag} has no manifest.json.");
        var tmp = System.IO.Path.GetTempFileName();
        try
        {
            await Download(h, a.Repo, asset.Id, tmp, asset.Size, null, ct);
            return JsonSerializer.Deserialize<Manifest>(await File.ReadAllTextAsync(tmp, ct)) ?? throw new IOException("Empty manifest.");
        }
        finally { File.Delete(tmp); }
    }

    // ---------------------------------------------------------------- comparing with the disk
    /// <summary>Hashes of files already checked, keyed by path, size and write time (hashing 16 GB each time is slow).</summary>
    sealed class HashCache { public Dictionary<string, string> Entries { get; set; } = new(); }
    static string CacheFile(string root) => System.IO.Path.Combine(root, "launcher", "data", "install_hashes.json");
    static HashCache LoadCache(string root)
    {
        try { var f = CacheFile(root); if (File.Exists(f)) return JsonSerializer.Deserialize<HashCache>(File.ReadAllText(f)) ?? new(); } catch (Exception) { }
        return new HashCache();
    }
    static void SaveCache(string root, HashCache c)
    {
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(CacheFile(root))!);
        File.WriteAllText(CacheFile(root), JsonSerializer.Serialize(c));
    }
    static string Sha256Of(string file)
    {
        using var s = File.OpenRead(file);
        return Convert.ToHexString(SHA256.HashData(s)).ToLowerInvariant();
    }
    static bool Matches(string root, FileEntry e, HashCache cache)
    {
        var full = System.IO.Path.Combine(root, e.Path.Replace('/', System.IO.Path.DirectorySeparatorChar));
        var fi = new FileInfo(full);
        if (!fi.Exists || fi.Length != e.Size) return false;
        var key = $"{e.Path}|{fi.Length}|{fi.LastWriteTimeUtc.Ticks}";
        if (!cache.Entries.TryGetValue(key, out var sha)) { sha = Sha256Of(full); cache.Entries[key] = sha; }
        return sha == e.Sha256;
    }

    /// <summary>The release last installed in full (launcher\data\installed_release.txt). The update notice at start
    /// compares releases, not files: files the game itself changes while playing aren't an update.</summary>
    static string InstalledFile(string root) => System.IO.Path.Combine(root, "launcher", "data", "installed_release.txt");
    public static string? InstalledRelease(string root)
    {
        try { var f = InstalledFile(root); return File.Exists(f) ? File.ReadAllText(f).Trim() : null; } catch (Exception) { return null; }
    }
    static void SetInstalledRelease(string root, string version)
    {
        try { Directory.CreateDirectory(System.IO.Path.GetDirectoryName(InstalledFile(root))!); File.WriteAllText(InstalledFile(root), version); } catch (Exception) { }
    }
    /// <summary>The running launcher differs from the release's.</summary>
    public static bool LauncherOutdated(Manifest m) =>
        m.Launcher != null && m.Launcher.Sha256.Length > 0 && !string.Equals(Sha256Of(Environment.ProcessPath!), m.Launcher.Sha256, StringComparison.OrdinalIgnoreCase);

    public sealed record Plan(Manifest Manifest, List<Pack> Packs, bool LauncherUpdate)
    {
        public long DownloadBytes => Packs.Sum(p => p.Size);
        public bool UpToDate => Packs.Count == 0 && !LauncherUpdate;
    }

    /// <summary>What has to be downloaded to bring <paramref name="root"/> up to the manifest.</summary>
    public static Plan Compare(string root, Manifest m, Action<string>? status = null)
    {
        var cache = LoadCache(root);
        var need = new List<Pack>();
        int i = 0;
        foreach (var p in m.Packs)
        {
            status?.Invoke($"Checking installed files ({++i}/{m.Packs.Count}: {p.Name})...");
            if (p.Files.Any(f => !Matches(root, f, cache))) need.Add(p);
        }
        SaveCache(root, cache);
        if (need.Count == 0) SetInstalledRelease(root, m.Version);
        return new Plan(m, need, LauncherOutdated(m));
    }

    /// <summary>Downloads and unpacks what <paramref name="plan"/> lists. Progress: (bytes done, bytes total, text).</summary>
    public static async Task Apply(string root, Account a, Plan plan, Action<long, long, string> progress, CancellationToken ct = default)
    {
        using var h = Client(a.Token);
        var releases = new Dictionary<string, GhRelease>();
        async Task<GhAsset> AssetOf(string tag, string name)
        {
            if (!releases.TryGetValue(tag, out var rel)) releases[tag] = rel = await Release(h, a.Repo, tag, ct);
            return rel.Assets.FirstOrDefault(x => x.Name == name) ?? throw new IOException($"{name} is missing from release {tag}.");
        }
        long total = plan.DownloadBytes + (plan.LauncherUpdate ? plan.Manifest.Launcher!.Size : 0), done = 0;
        var tmpDir = System.IO.Path.Combine(root, "launcher", "data", "update_tmp");
        Directory.CreateDirectory(tmpDir);
        var free = new DriveInfo(System.IO.Path.GetPathRoot(System.IO.Path.GetFullPath(root))!).AvailableFreeSpace;
        long needSpace = plan.Packs.Sum(p => p.Files.Sum(f => f.Size)) + (plan.Packs.Count > 0 ? plan.Packs.Max(p => p.Size) : 0);
        if (free < needSpace) throw new IOException($"Not enough free space on {System.IO.Path.GetPathRoot(root)}: {needSpace / 1e9:0.0} GB needed, {free / 1e9:0.0} GB free.");
        var cache = LoadCache(root);
        foreach (var p in plan.Packs)
        {
            var asset = await AssetOf(p.Release, p.Asset);
            var zip = System.IO.Path.Combine(tmpDir, p.Asset);
            long start = done;
            await Download(h, a.Repo, asset.Id, zip, p.Size, got => progress(start + got, total, $"Downloading {p.Name} ({got / 1e6:0} / {p.Size / 1e6:0} MB)"), ct);
            progress(start + p.Size, total, $"Checking {p.Name}...");
            if (p.Sha256.Length > 0 && Sha256Of(zip) != p.Sha256) { File.Delete(zip); throw new IOException($"{p.Asset} arrived damaged (checksum mismatch); try again."); }
            progress(start + p.Size, total, $"Unpacking {p.Name}...");
            using (var za = ZipFile.OpenRead(zip))
                foreach (var e in za.Entries)
                {
                    if (e.FullName.EndsWith("/")) continue;
                    var dest = System.IO.Path.GetFullPath(System.IO.Path.Combine(root, e.FullName.Replace('/', System.IO.Path.DirectorySeparatorChar)));
                    if (!dest.StartsWith(System.IO.Path.GetFullPath(root), StringComparison.OrdinalIgnoreCase)) continue;   // stay inside the install
                    Directory.CreateDirectory(System.IO.Path.GetDirectoryName(dest)!);
                    e.ExtractToFile(dest, true);
                    var fe = p.Files.FirstOrDefault(f => f.Path == e.FullName);
                    if (fe != null) { var fi = new FileInfo(dest); cache.Entries[$"{fe.Path}|{fi.Length}|{fi.LastWriteTimeUtc.Ticks}"] = fe.Sha256; }
                }
            File.Delete(zip);
            SaveCache(root, cache);
            done = start + p.Size;
        }
        if (plan.LauncherUpdate)
        {
            var l = plan.Manifest.Launcher!;
            var asset = await AssetOf(l.Release, l.Asset);
            var exe = Environment.ProcessPath!;
            var fresh = exe + ".new";
            long start = done;
            await Download(h, a.Repo, asset.Id, fresh, l.Size, got => progress(start + got, total, $"Downloading the new launcher ({got / 1e6:0} / {l.Size / 1e6:0} MB)"), ct);
            if (Sha256Of(fresh) != l.Sha256) { File.Delete(fresh); throw new IOException("The new launcher arrived damaged; try again."); }
            // a running exe can be renamed but not overwritten: move it aside, put the new one in its place
            var old = exe + ".old";
            if (File.Exists(old)) File.Delete(old);
            File.Move(exe, old);
            File.Move(fresh, exe);
        }
        try { Directory.Delete(tmpDir, true); } catch (Exception) { }
        SetInstalledRelease(root, plan.Manifest.Version);
        progress(total, total, "Up to date.");
    }

    /// <summary>Restarts the (new) launcher.</summary>
    public static void Restart()
    {
        Process.Start(new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = true });
        System.Windows.Application.Current.Shutdown();
    }

    /// <summary>The previous launcher, left by a self-update, is deleted on the next start.</summary>
    public static void CleanUpOldLauncher()
    {
        try { var old = Environment.ProcessPath + ".old"; if (File.Exists(old)) File.Delete(old); } catch (Exception) { }
    }
}
