using System.Configuration;
using System.Data;
using System;
using System.IO;
using System.Linq;
using System.Windows;

namespace MNRLauncher;

/// <summary>
/// Interaction logic for App.xaml
/// </summary>
public partial class App : Application
{
    /// <summary>MNRLauncher.exe --play (tests): no window; starts the game as PLAY would, with the saved settings (online
    /// sign-in and its ticket service included) and the game's output in logs\last_run.txt, and exits with it. Variables
    /// set for the launcher (MNR_HEADLESS=1, MNR_PAD_SCRIPT=...) reach the game.</summary>
    protected override void OnStartup(StartupEventArgs e)
    {
        if (e.Args.Length >= 2 && (e.Args[0] == "--install-pkg" || e.Args[0] == "--list-pkg"))
        {   // (tests) install or list a DLC package without the window; output goes to pkg_install.txt next to the launcher data
            var log = new StringWriter();
            try
            {
                Console.SetOut(log);
                var r = MNRLauncher.Core.PkgInstaller.Install(e.Args[1], MNRLauncher.Core.Paths.Hdd0, e.Args[0] == "--list-pkg");
                log.WriteLine($"{r.ContentId} -> {r.TitleId}: {r.Files} files");
            }
            catch (Exception ex) { log.WriteLine("ERROR " + ex.Message); }
            File.WriteAllText(Path.Combine(MNRLauncher.Core.Paths.LauncherData, "pkg_install.txt"), log.ToString());
            Shutdown(0); return;
        }
        if (e.Args.Length >= 3 && e.Args[0] == "--mod-pkg")
        {   // (tests) import a mod package into the folder given (not the real mods folder); output as above
            var log = new StringWriter();
            try
            {
                var r = MNRLauncher.Core.PkgInstaller.ExtractMod(e.Args[1], e.Args[2]);
                log.WriteLine($"{r.Name}: {r.Files} files in {r.Folder}");
                foreach (var s in r.Skipped) log.WriteLine("skipped " + s);
                log.WriteLine("dlc=" + MNRLauncher.Core.PkgInstaller.IsDlc(e.Args[1]));
            }
            catch (Exception ex) { log.WriteLine("ERROR " + ex.Message); }
            File.WriteAllText(Path.Combine(MNRLauncher.Core.Paths.LauncherData, "pkg_install.txt"), log.ToString());
            Shutdown(0); return;
        }
        if (e.Args.Contains("--play"))
        {
            ShutdownMode = ShutdownMode.OnExplicitShutdown;
            var s = MNRLauncher.Core.Settings.Load();
            s.KeepConsole = false;   // (not saved) no console window: the log goes to the file
            var mods = MNRLauncher.Core.ModManager.Scan(s).Where(m => m.Enabled).OrderBy(m => s.EnabledMods.IndexOf(m.Id)).ToList();
            try { foreach (var n in System.Threading.Tasks.Task.Run(() => MNRLauncher.Core.CreationSync.Prepare(s)).GetAwaiter().GetResult()) Console.WriteLine("[creations] " + n); } catch (Exception) { }
            var p = MNRLauncher.Core.GameRunner.Start(s, mods);
            p.EnableRaisingEvents = true;
            p.Exited += (_, _) => { System.Threading.Thread.Sleep(1500); Dispatcher.Invoke(() => Shutdown(p.ExitCode)); };
            return;
        }
        if ((e.Args.Length >= 3 && e.Args[0] == "--install-creation") || (e.Args.Length >= 2 && e.Args[0] == "--prepare"))
        {   // (tests) --install-creation <out.txt> <id>[,<id>...] | --prepare <out.txt>: CreationDownload / CreationSync with the saved settings
            var o = new StringWriter();
            try
            {
                var s = MNRLauncher.Core.Settings.Load();
                if (e.Args[0] == "--prepare") foreach (var n in System.Threading.Tasks.Task.Run(() => MNRLauncher.Core.CreationSync.Prepare(s, t => o.WriteLine("  " + t))).GetAwaiter().GetResult()) o.WriteLine(n);
                else foreach (var id in e.Args[2].Split(','))
                    o.WriteLine($"{id}: {System.Threading.Tasks.Task.Run(() => MNRLauncher.Core.CreationDownload.Install(s, int.Parse(id), t => o.WriteLine("  " + t))).GetAwaiter().GetResult()}");
            }
            catch (Exception ex) { o.WriteLine("ERROR " + ex.Message); }
            File.WriteAllText(e.Args[1], o.ToString());
            Shutdown(0); return;
        }
        if (e.Args.Length == 4 && e.Args[0] == "--public-setup")
        {   // (tests) --public-setup <disc> <update.pkg> <log.txt>: the Setup page's steps without the window (MNR_ROOT: the install)
            var log = new StringWriter(); var t0 = DateTime.Now;
            void Rep(string s, double f) { lock (log) log.WriteLine($"{(DateTime.Now - t0).TotalSeconds,6:0}s {s}"); }
            try
            {   // (on the thread pool: blocking the UI thread on the steps' awaits deadlocks)
                System.Threading.Tasks.Task.Run(async () =>
                {
                    MNRLauncher.Core.Paths.EnsureFolders();
                    if (!MNRLauncher.Core.GameSetup.RuntimePresent) await MNRLauncher.Core.GameSetup.FetchRuntime(Rep, default);
                    await MNRLauncher.Core.GameSetup.InstallDisc(e.Args[1], Rep, default);
                    await MNRLauncher.Core.GameSetup.InstallUpdate(e.Args[2], Rep, default);
                    await MNRLauncher.Core.GameSetup.EnsureTools(Rep, default);
                    await MNRLauncher.Core.GameSetup.Build(Rep, s => Rep(s, -1), default);
                    Rep("complete: " + MNRLauncher.Core.GameSetup.Complete, 1);
                }).GetAwaiter().GetResult();
            }
            catch (Exception ex) { Rep("FAILED: " + ex, -1); }
            lock (log) File.WriteAllText(e.Args[3], log.ToString());
            Shutdown(); return;
        }
        if (e.Args.Length == 3 && e.Args[0] == "--iso-extract")
        {   // (tests) --iso-extract <disc.iso> <folder>: copy the disc's PS3_GAME, PS3_UPDATE and PS3_DISC.SFB out of an image
            try { using var iso = new MNRLauncher.Core.IsoReader(e.Args[1]); iso.Extract(e.Args[2], null, default); }
            catch (Exception ex) { File.WriteAllText(Path.Combine(e.Args[2], "error.txt"), ex.ToString()); }
            Shutdown(); return;
        }
        if (e.Args.Length == 3 && e.Args[0] == "--pkg-install")
        {   // (tests) --pkg-install <package.pkg> <hdd0 folder>: install a package as the public edition's setup does
            try { var r = MNRLauncher.Core.PkgInstaller.Install(e.Args[1], e.Args[2]); File.WriteAllText(Path.Combine(e.Args[2], "result.txt"), $"{r.ContentId} {r.TitleId} {r.Files} files"); }
            catch (Exception ex) { File.WriteAllText(Path.Combine(e.Args[2], "error.txt"), ex.ToString()); }
            Shutdown(); return;
        }
        if (e.Args.Length == 3 && e.Args[0] == "--unself")
        {   // (tests) --unself <EBOOT.BIN> <out.elf>: decrypt the game's executable as the public edition's setup does
            try { File.WriteAllBytes(e.Args[2], MNRLauncher.Core.SelfDecrypter.Decrypt(File.ReadAllBytes(e.Args[1]))); }
            catch (Exception ex) { File.WriteAllText(e.Args[2] + ".error.txt", ex.ToString()); }
            Shutdown(); return;
        }
        if (e.Args.Length >= 2 && e.Args[0] == "--rpcn-friends")
        {   // (tests) --rpcn-friends <out.txt> [<npid>...]: the saved account's RPCN friends (and requests), after sending a friend request to each npid given
            var o = new StringWriter();
            try
            {
                var s = MNRLauncher.Core.Settings.Load();
                using var c = MNRLauncher.Core.RpcnClient.Connect(string.IsNullOrWhiteSpace(s.RpcnHost) ? MNRLauncher.Core.RpcnClient.DefaultHost : s.RpcnHost);
                c.Login(s.RpcnUser.Trim(), s.RpcnPassword, s.RpcnToken.Trim());
                o.WriteLine("signed in as " + c.OnlineName);
                foreach (var n in e.Args.Skip(2)) o.WriteLine($"friend request to {n}: {MNRLauncher.Core.RpcnClient.ErrorName(c.AddFriend(n))}");
                lock (c.Friends)
                {
                    foreach (var (n, f) in c.Friends) o.WriteLine($"friend {n} {MNRLauncher.Core.RpcnClient.PresenceLine(f)}");
                    foreach (var n in c.RequestsReceived) o.WriteLine("request from " + n);
                    foreach (var n in c.RequestsSent) o.WriteLine("request to " + n);
                }
            }
            catch (Exception ex) { o.WriteLine("ERROR " + ex.Message); }
            File.WriteAllText(e.Args[1], o.ToString());
            Shutdown(0); return;
        }
        if (e.Args.Length >= 2 && e.Args[0] == "--selftest")
        {   // (tests) --selftest <out.txt>: the creation check, a bug report, a save backup and the crash check, reported
            var o = new StringWriter();
            try
            {
                var s = MNRLauncher.Core.Settings.Load();
                o.WriteLine("== check"); foreach (var l in MNRLauncher.Core.CreationInstaller.Check()) o.WriteLine(l);
                o.WriteLine("== bug report " + MNRLauncher.Core.BugReport.Make());
                o.WriteLine("== backup " + MNRLauncher.Core.SaveBackup.Make(MNRLauncher.Core.Profiles.Active(s), "test"));
                var c = MNRLauncher.Core.BugReport.LastCrash();
                o.WriteLine("== last crash " + (c == null ? "none" : $"{c.Value.file} {c.Value.when}"));
                o.WriteLine("== installed release " + MNRLauncher.Core.Updater.InstalledRelease(MNRLauncher.Core.Paths.Root));
                o.WriteLine("== codec " + MNRLauncher.Core.PcSettings.VoiceCodec().state + "; mics: " + string.Join(" | ", MNRLauncher.Core.PcSettings.Microphones()));
            }
            catch (Exception ex) { o.WriteLine("ERROR " + ex); }
            File.WriteAllText(e.Args[1], o.ToString());
            Shutdown(0); return;
        }
        if (e.Args.Length >= 2 && e.Args[0] == "--shot")
        {   // (tests) --shot <folder> [theme]: every sidebar page drawn off-screen to <folder>\<n>_<page>.png, then exit
            MNRLauncher.Core.Themes.Apply(e.Args.Length > 2 ? e.Args[2] : MNRLauncher.Core.Settings.Load().Theme);
            ShutdownMode = ShutdownMode.OnExplicitShutdown;
            var w = new MainWindow { Left = -20000, Top = -20000, ShowActivated = false, ShowInTaskbar = false, WindowStartupLocation = WindowStartupLocation.Manual };
            w.Show();
            w.SaveShots(e.Args[1]);
            w.Close(); Shutdown(0); return;
        }
        try { MNRLauncher.Core.Themes.Apply(MNRLauncher.Core.Settings.Load().Theme); } catch (Exception) { }   // before the window: no flash of the default colours
        StartupUri = new Uri("MainWindow.xaml", UriKind.Relative);
        base.OnStartup(e);
    }
}

