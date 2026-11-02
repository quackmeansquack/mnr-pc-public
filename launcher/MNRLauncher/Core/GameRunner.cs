using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;

namespace MNRLauncher.Core;

/// <summary>Starts the native build with the launcher's settings turned into its environment switches.</summary>
public static class GameRunner
{
    public static Process? Running { get; private set; }
    public static bool IsRunning => Running is { HasExited: false };

    public static string? Problem()
    {
        if (!File.Exists(Paths.GameExe)) return $"The game isn't installed in {Paths.Root} yet: open Install & updates.";
        if (!File.Exists(Paths.Eboot)) return $"The decrypted 1.11 EBOOT is missing:\n{Paths.Eboot}";
        if (!Directory.Exists(Paths.GameData)) return $"The game files are missing:\n{Paths.GameData}";
        // builds before the universal one (no march.txt next to mnr.exe) need a Haswell-class processor
        if (Paths.GameExeMarch == "haswell" && CpuProblem() is string cpu) return cpu + "\n\nInstall & updates downloads the build for this processor.";
        return null;
    }


    /// <summary>mnr.exe is compiled for Haswell-class processors (AVX2, FMA, BMI1/2, LZCNT, MOVBE: Intel Core 4th
    /// generation (2013) and newer, AMD Excavator/Ryzen and newer); on older ones it stops at its first instruction.</summary>
    public static string? CpuProblem()
    {
        var missing = new List<string>();
        try
        {
            if (!System.Runtime.Intrinsics.X86.Avx2.IsSupported) missing.Add("AVX2");
            if (!System.Runtime.Intrinsics.X86.Fma.IsSupported) missing.Add("FMA");
            if (!System.Runtime.Intrinsics.X86.Bmi1.IsSupported) missing.Add("BMI1");
            if (!System.Runtime.Intrinsics.X86.Bmi2.IsSupported) missing.Add("BMI2");
            if (!System.Runtime.Intrinsics.X86.Lzcnt.IsSupported) missing.Add("LZCNT");
            if (System.Runtime.Intrinsics.X86.X86Base.IsSupported && (System.Runtime.Intrinsics.X86.X86Base.CpuId(1, 0).Ecx & (1 << 22)) == 0) missing.Add("MOVBE");
        }
        catch (Exception) { return null; }
        return missing.Count == 0 ? null :
            $"This processor lacks instructions the game build needs ({string.Join(", ", missing)}).\n\nThe build needs an Intel Core 4th generation (2013) or newer, or an AMD Ryzen / FX Excavator or newer.";
    }

    /// <summary>What an early exit code means (the game shows no window when it stops this early).</summary>
    public static string ExitMeaning(int code) => unchecked((uint)code) switch
    {
        0xC000001D => "The processor doesn't support an instruction the game uses (it needs AVX2: Intel Core 4th generation / AMD Ryzen or newer).",
        0xC0000135 => "A Windows component (DLL) the game needs is missing. Install the latest Windows updates; the game needs Direct3D 11 (d3d11.dll, D3DCompiler_47.dll) and the Universal C Runtime.",
        0xC0000139 or 0xC0000138 => "A Windows component (DLL) the game needs is too old. Install the latest Windows updates.",
        0xC0000005 => "The game crashed (access violation).",
        0xC00000FD => "The game crashed (stack overflow).",
        0xC0000017 or 0xC000012D => "Not enough memory: the game reserves about 4.5 GB of address space and needs several GB of free RAM plus page file.",
        3 => "Direct3D 11 couldn't start: the graphics card or its driver doesn't support Direct3D 11 (feature level 11.0). Update the graphics driver.",
        _ => $"The game stopped with exit code 0x{code:X8}.",
    };

    /// <summary>Raised (on a worker thread) when the game exits within 20 s of starting: the explanation and the end of the log.</summary>
    public static event Action<string>? EarlyExit;

    static TicketService? tickets;
    static Settings? ticketSettings;
    /// <summary>The local service that gives the game RPCN tickets (started on first use, kept for the launcher's life).</summary>
    public static TicketService Tickets(Settings s)
    {
        ticketSettings = s;
        return tickets ??= new TicketService(() => (ticketSettings!.RpcnHost, ticketSettings.RpcnUser.Trim(), ticketSettings.RpcnPassword, ticketSettings.RpcnToken.Trim()));
    }

    /// <summary>The server address as an IPv4 address: a name like mnr.quackcraft.net is looked up here (the game's
    /// host map takes addresses); left as typed if it is already an address or the lookup fails.</summary>
    static string ServerIp(string address)
    {
        if (System.Net.IPAddress.TryParse(address, out _)) return address;
        try
        {
            var ip = System.Net.Dns.GetHostAddresses(address).FirstOrDefault(a => a.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork);
            return ip?.ToString() ?? address;
        }
        catch (Exception) { return address; }
    }

    public static Dictionary<string, string> Environment(Settings s, IEnumerable<Mod> enabledMods)
    {
        var env = new Dictionary<string, string>
        {
            ["MNR_HLE_LENIENT"] = "1",   // older builds stopped at the first unimplemented system call (as run_mnr.bat sets)
        };
        var profile = Profiles.Active(s);
        if (!profile.IsDefault) env["MNR_SAVEDATA_DIR"] = profile.Folder;
        if (s.FpsCap != 30) env["MNR_FPS_CAP"] = s.FpsCap.ToString();
        if (!s.ShowFps) env["MNR_NO_FPS"] = "1";
        if (s.Mute) env["MNR_MUTE"] = "1";
        if (s.DetailedLog) { env["MNR_GAME_LOG"] = "1"; env["MNR_SAVEDATA_LOG"] = "1"; }
        if (!s.UseController) env["MNR_NO_XINPUT"] = "1";
        if (s.Resolution > 720) env["MNR_RESOLUTION"] = s.Resolution.ToString();   // graphics (rsx_d3d.c)
        if (s.Anisotropy > 1) env["MNR_ANISO"] = s.Anisotropy.ToString();
        if (s.Fxaa) env["MNR_FXAA"] = "1";
        if (ControlMap.Load().Count > 0) env["MNR_INPUT_CONFIG"] = ControlMap.File;   // wheels, pedals, ...
        var (mods, patches) = ModManager.Environment(enabledMods);
        if (mods.Length > 0) env["MNR_MODS"] = mods;
        if (s.UnlockAllTracks) env["MNR_UNLOCK_TRACKS"] = "1";   // the game marks every career track unlocked (host.c)
        if (s.AllowRemix) env["MNR_ALLOW_REMIX"] = "1";   // the studios open anyone's creations (native.c)
        try { CreationInstaller.UndoRemixPatches(); } catch (Exception) { }   // .CMD bytes an earlier launcher changed
        if (patches.Length > 0) env["MNR_MOD_PATCHES"] = patches;
        // for the game's network layer (as RPCS3's "IP/hosts switches": modnation.ps3.playstationcloud.com=45.32.179.33)
        if (!string.IsNullOrWhiteSpace(s.ServerAddress))   // an empty host name means the game's own server name
        {
            var host = string.IsNullOrWhiteSpace(s.ServerHost) ? "modnation.ps3.playstationcloud.com" : s.ServerHost.Trim();
            env["MNR_HOSTS"] = $"{host}={ServerIp(s.ServerAddress.Trim())}";
        }
        if (s.OnlineEnabled)
        {   // sockets on, and the PSN sign-in answered with tickets from the player's RPCN account (Rpcn.cs)
            env["MNR_NET"] = "1";
            if (s.RacePort > 0 && s.RacePort != 3658) env["MNR_P2P_PORT"] = s.RacePort.ToString();   // this PC's race port
            if (s.DetailedLog) env["MNR_NET_LOG"] = "1";   // connections and transfers in the bug-report log
            if (s.DetailedLog) env["MNR_P2P_LOG"] = "1";   // player-to-player port and signaling (p2p.c)
            if (!string.IsNullOrWhiteSpace(s.RpcnUser) && !string.IsNullOrEmpty(s.RpcnPasswordProtected))
            {
                env["MNR_NP_ID"] = s.RpcnUser.Trim();
                env["MNR_NP_HELPER"] = $"127.0.0.1:{Tickets(s).Port}";
            }
        }
        foreach (var line in s.ExtraEnvironment.Split('\n').Select(l => l.Trim()).Where(l => l.Length > 0 && !l.StartsWith("#")))
        {
            int eq = line.IndexOf('=');
            if (eq > 0) env[line[..eq].Trim()] = line[(eq + 1)..].Trim();
        }
        return env;
    }

    public static Process Start(Settings s, IEnumerable<Mod> enabledMods)
    {
        bool toFile = !s.KeepConsole || s.DetailedLog;   // a detailed log always goes to the file
        var psi = new ProcessStartInfo
        {
            FileName = Paths.GameExe,
            Arguments = $"\"{Paths.Eboot}\" --boot",
            WorkingDirectory = Paths.ReDir,   // the shader cache lives here, shared with run_mnr.bat
            UseShellExecute = false,
            CreateNoWindow = toFile,
            RedirectStandardOutput = toFile,
            RedirectStandardError = toFile,
        };
        foreach (var kv in Environment(s, enabledMods)) psi.Environment[kv.Key] = kv.Value;
        if (!toFile)
        {   // the log window is showing: the game also copies its output to MNR\logs\last_run.txt
            Directory.CreateDirectory(Paths.LogsDir);
            psi.Environment["MNR_LOG_FILE"] = Path.Combine(Paths.LogsDir, "last_run.txt");
        }
        var p = Process.Start(psi) ?? throw new InvalidOperationException("The game didn't start.");
        if (toFile)
        {   // keep the log in MNR\logs\last_run.txt
            Directory.CreateDirectory(Paths.LogsDir);
            // written in blocks and flushed twice a second (flushing every line kept the game waiting on a busy log)
            var log = new StreamWriter(new FileStream(Path.Combine(Paths.LogsDir, "last_run.txt"), FileMode.Create, FileAccess.Write, FileShare.ReadWrite, 1 << 16)) { AutoFlush = false };
            object gate = new(); bool closed = false;
            void Write(string? line) { if (line == null) return; lock (gate) { if (!closed) try { log.WriteLine(line); } catch (Exception) { } } }
            var flusher = new System.Threading.Timer(_ => { lock (gate) { if (!closed) try { log.Flush(); } catch (Exception) { } } }, null, 500, 500);
            p.OutputDataReceived += (_, e) => Write(e.Data);
            p.ErrorDataReceived += (_, e) => Write(e.Data);
            p.EnableRaisingEvents = true;
            p.Exited += (_, _) =>
            {
                System.Threading.Thread.Sleep(300);   // the last lines are still on their way through the pipe
                lock (gate) { closed = true; flusher.Dispose(); try { log.Dispose(); } catch (Exception) { } }
            };
            p.BeginOutputReadLine(); p.BeginErrorReadLine();
        }
        var started = DateTime.Now;
        p.EnableRaisingEvents = true;
        p.Exited += (_, _) => tickets?.EndSession();   // sign out of RPCN with the game
        p.Exited += (_, _) =>
        {
            int code; try { code = p.ExitCode; } catch { return; }
            if (code == 0 || DateTime.Now - started > TimeSpan.FromSeconds(20)) return;
            string tail = "";
            if (toFile)
            {
                System.Threading.Thread.Sleep(500);   // let the log writer finish
                try
                {
                    using var fs = new FileStream(Path.Combine(Paths.LogsDir, "last_run.txt"), FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                    var lines = new StreamReader(fs).ReadToEnd().Split('\n').Select(l => l.TrimEnd()).Where(l => l.Length > 0).TakeLast(12);
                    tail = "\n\nLast lines of logs\\last_run.txt:\n" + string.Join("\n", lines);
                }
                catch (Exception) { }
            }
            else tail = "\n\nFor details, untick \"Show the log window\" in Settings (the log then goes to logs\\last_run.txt) and try again.";
            EarlyExit?.Invoke(ExitMeaning(code) + tail);
        };
        Running = p;
        return p;
    }
}
