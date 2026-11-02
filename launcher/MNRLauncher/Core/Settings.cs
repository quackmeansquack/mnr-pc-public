using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace MNRLauncher.Core;

/// <summary>Launcher settings, saved as JSON in MNR\launcher\data\settings.json.</summary>
public class Settings
{
    // Game
    public int FpsCap { get; set; } = 30;              // the console's cap; 60 is experimental
    public bool ShowFps { get; set; } = true;
    public bool Mute { get; set; }
    public bool UseController { get; set; } = true;
    public bool KeepConsole { get; set; } = true;      // show the log console next to the game
    public bool DetailedLog { get; set; }              // game + save messages to logs\last_run.txt (bug reports)
    public bool UnlockAllTracks { get; set; }          // every career track unlocked (MNR_UNLOCK_TRACKS, host.c), for online races
    public bool AllowRemix { get; set; } = true;       // the studios open downloaded creations whatever their creators chose (MNR_ALLOW_REMIX, native.c), to remix and republish
    public string ExtraEnvironment { get; set; } = ""; // advanced: NAME=value per line
    public string Theme { get; set; } = Themes.Default;   // the launcher's colours (Core\Themes.cs)
    // Graphics (rsx_d3d.c)
    public int Resolution { get; set; } = 720;            // the main scene's height: 720, 1080, 1440, 2160 (MNR_RESOLUTION)
    public int Anisotropy { get; set; }                    // 0 off, 2..16 (MNR_ANISO)
    public bool Fxaa { get; set; }                         // smoothed edges (MNR_FXAA)
    public string CreationsServer { get; set; } = "";      // the server Data\Upload and Data\Download belong to (CreationSync)
    public string CrashSeen { get; set; } = "";            // the crashed session's log the Play page's notice was dismissed for
    public string LatestRelease { get; set; } = "";        // the newest release seen at start, and its notes (Play page: what's new)
    public string LatestNotes { get; set; } = "";

    // Save profile in use ("" = the game's normal saves); see Profiles
    public string ActiveProfile { get; set; } = "";

    // Mods: folder names in load order (first wins when two mods replace the same file)
    public List<string> EnabledMods { get; set; } = new();

    // Online
    public string BrowserUrl { get; set; } = "https://mnrrevival.derole.net";   // creation browser (MNR Revival)
    public string ServerHost { get; set; } = "modnation.ps3.playstationcloud.com";
    public string ServerAddress { get; set; } = "mnr.quackcraft.net";   // our own server (MNR\server); the revival server is 45.32.179.33
    public int SettingsVersion { get; set; }           // 1: the old default server address was moved to ours
    public int CompositeCacheReset { get; set; }       // 1: the game's composite cache was cleared once (MainWindow)
    public int ServerPort { get; set; }                // 0: the default HTTP port
    public string PlayerName { get; set; } = "";
    public bool OnlineEnabled { get; set; }            // sign in with RPCN and let the game reach the servers (MNR_NET)
    public int RacePort { get; set; } = 3658;          // UDP port for online races (the console's 3658); two PCs on one network need different ones
    public string RpcnHost { get; set; } = "np.rpcs3.net:31313";
    public string RpcnUser { get; set; } = "";
    public string RpcnPasswordProtected { get; set; } = "";   // RPCS3's derived password, encrypted for this Windows user (DPAPI)
    public string RpcnToken { get; set; } = "";        // e-mail verification token (only if the server asks for one)

    // Saved accounts (see Accounts): the one in use is copied into the RPCN fields above
    public List<Account> Accounts { get; set; } = new();
    public string ActiveAccount { get; set; } = "";

    [System.Text.Json.Serialization.JsonIgnore]
    public string RpcnPassword
    {
        get => Unprotect(RpcnPasswordProtected);
        set => RpcnPasswordProtected = Protect(value);
    }

    /// <summary>Encrypted for this Windows user (DPAPI); "" stays "".</summary>
    public static string Protect(string value) => string.IsNullOrEmpty(value) ? "" :
        Convert.ToBase64String(System.Security.Cryptography.ProtectedData.Protect(System.Text.Encoding.UTF8.GetBytes(value), null, System.Security.Cryptography.DataProtectionScope.CurrentUser));
    public static string Unprotect(string value)
    {
        if (string.IsNullOrEmpty(value)) return "";
        try { return System.Text.Encoding.UTF8.GetString(System.Security.Cryptography.ProtectedData.Unprotect(Convert.FromBase64String(value), null, System.Security.Cryptography.DataProtectionScope.CurrentUser)); }
        catch (Exception) { return ""; }
    }

    public static Settings Load()
    {
        try
        {
            if (File.Exists(Paths.SettingsFile))
            {
                var s = JsonSerializer.Deserialize<Settings>(File.ReadAllText(Paths.SettingsFile)) ?? new Settings();
                if (s.SettingsVersion < 1)   // still on the old default (the revival server): move to ours, once
                {
                    if (s.ServerAddress.Trim() == "45.32.179.33") s.ServerAddress = "mnr.quackcraft.net";
                    s.SettingsVersion = 1;
                }
                if (s.SettingsVersion < 2)   // graphics switches typed as extra environment lines become the Graphics settings, once
                {
                    var keep = new List<string>();
                    foreach (var raw in s.ExtraEnvironment.Split('\n'))
                    {
                        var line = raw.Trim(); int eq = line.IndexOf('=');
                        string k = eq > 0 ? line[..eq].Trim() : "", v = eq > 0 ? line[(eq + 1)..].Trim() : "";
                        if (k == "MNR_RESOLUTION" && int.TryParse(v, out var r) && r >= 720) s.Resolution = r;
                        else if (k == "MNR_ANISO" && int.TryParse(v, out var a)) s.Anisotropy = a > 1 ? Math.Min(a, 16) : 0;
                        else if (k == "MNR_FXAA") s.Fxaa = v != "0";
                        else if (line.Length > 0) keep.Add(line);
                    }
                    s.ExtraEnvironment = string.Join("\r\n", keep);
                    s.SettingsVersion = 2;
                }
                return s;
            }
        }
        catch (Exception) { /* a broken file falls back to defaults */ }
        return new Settings();
    }

    public void Save()
    {
        Directory.CreateDirectory(Paths.LauncherData);
        File.WriteAllText(Paths.SettingsFile, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
    }
}
