using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text.Json.Nodes;

namespace MNRLauncher.Core;

/// <summary>A zip of what a bug report needs: the last few game logs, the end of the RPCN log, the launcher's and the
/// game's settings with every password and token blanked, and the versions. Saved in logs\ and shown in Explorer.</summary>
public static class BugReport
{
    /// <summary>JSON keys whose values are blanked when including launcher settings in the bug report.</summary>
    static readonly string[] Secret = { "RpcnPasswordProtected", "RpcnToken", "TokenProtected" };

    /// <summary>
    /// Creates a zip file containing: the last 3 game logs, last_run.txt, the last 200 lines of rpcn.log,
    /// pc_settings.ini, launcher_settings.json (with passwords/tokens blanked), and version info.
    /// Returns the path to the generated zip file.
    /// </summary>
    public static string Make()
    {
        var zipPath = Path.Combine(Paths.LogsDir, $"bug_report_{DateTime.Now:yyyy-MM-dd_HH-mm}.zip");
        Directory.CreateDirectory(Paths.LogsDir);
        if (File.Exists(zipPath)) File.Delete(zipPath);
        using var z = ZipFile.Open(zipPath, ZipArchiveMode.Create);
        void Add(string file, string? name = null) { if (File.Exists(file)) z.CreateEntryFromFile(file, name ?? Path.GetFileName(file), CompressionLevel.Optimal); }
        void Text(string name, string text) { using var w = new StreamWriter(z.CreateEntry(name).Open()); w.Write(text); }

        foreach (var f in new DirectoryInfo(Paths.LogsDir).GetFiles("run_*.txt").OrderByDescending(f => f.LastWriteTimeUtc).Take(3)) Add(f.FullName);
        Add(Path.Combine(Paths.LogsDir, "last_run.txt"));
        var rpcn = Path.Combine(Paths.LogsDir, "rpcn.log");
        if (File.Exists(rpcn)) Text("rpcn.log", string.Join("\n", File.ReadAllLines(rpcn).TakeLast(200)));
        Add(PcSettings.File);
        if (File.Exists(Paths.SettingsFile))
        {
            try
            {
                var j = JsonNode.Parse(File.ReadAllText(Paths.SettingsFile))!;
                void Blank(JsonNode? n)
                {
                    if (n is JsonObject o) { foreach (var k in o.Select(p => p.Key).ToList()) { if (Secret.Contains(k)) o[k] = "(removed)"; else Blank(o[k]); } }
                    else if (n is JsonArray a) foreach (var x in a) Blank(x);
                }
                Blank(j);
                Text("launcher_settings.json", j.ToJsonString(new System.Text.Json.JsonSerializerOptions { WriteIndented = true }));
            }
            catch (Exception) { }
        }
        Text("versions.txt", $"launcher {MainWindow.LauncherVersion}\ninstalled release {Updater.InstalledRelease(Paths.Root) ?? "?"}\n" +
                             $"game exe {(File.Exists(Paths.GameExe) ? File.GetLastWriteTime(Paths.GameExe).ToString("yyyy-MM-dd HH:mm") : "missing")}\n" +
                             $"Windows {Environment.OSVersion.VersionString}, {Environment.ProcessorCount} logical cores\n");
        return zipPath;
    }

    /// <summary>The newest game log, if that session ended in a crash (host.c's crash handler, or a jump to code the
    /// recompiler doesn't have): its file name and when it was written.</summary>
    /// <summary>
    /// Checks the newest game log for crash markers (host exception or unrecompilable JIT function).
    /// Returns the log file name and timestamp if a crash is detected, null otherwise.
    /// </summary>
    public static (string file, DateTime when)? LastCrash()
    {
        try
        {
            var log = new DirectoryInfo(Paths.LogsDir).GetFiles("run_*.txt").Concat(new DirectoryInfo(Paths.LogsDir).GetFiles("last_run.txt"))
                          .OrderByDescending(f => f.LastWriteTimeUtc).FirstOrDefault();
            if (log == null) return null;
            using var s = new FileStream(log.FullName, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            s.Seek(Math.Max(0, s.Length - 256 * 1024), SeekOrigin.Begin);   // the end of the session
            var tail = new StreamReader(s).ReadToEnd();
            return tail.Contains("*** host exception") || tail.Contains("dispatch: no recompiled function") ? (log.Name, log.LastWriteTime) : null;
        }
        catch (Exception) { return null; }
    }

    /// <summary>Creates a bug report zip and opens Windows Explorer with it selected.</summary>
    public static void MakeAndShow()
    {
        try { var p = Make(); Process.Start("explorer.exe", $"/select,\"{p}\""); }
        catch (Exception ex) { System.Windows.MessageBox.Show(ex.Message, "Couldn't make the bug report"); }
    }
}
