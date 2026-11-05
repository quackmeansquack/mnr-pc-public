using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace MNRLauncher.Core;

/// <summary>
/// Launcher configuration stored as JSON in MNR\launcher\data\settings.json.
///
/// This class is the single source of truth for all user-facing settings. It handles:
/// - Persistence (Load/Save) with automatic migration for old formats.
/// - DPAPI encryption for the RPCN password (tied to the current Windows user).
/// - Versioned migrations (SettingsVersion) for backward compatibility.
/// </summary>
public class Settings
{
    // ==================== Game Settings ====================
    /// <summary>Frame rate cap. Default 30 matches the PS3 console; 60 is experimental and may cause stutter.</summary>
    public int FpsCap { get; set; } = 30;

    /// <summary>Show FPS overlay in the top-left corner of the game window.</summary>
    public bool ShowFps { get; set; } = true;

    /// <summary>Mute all game audio (voices, music, SFX).</summary>
    public bool Mute { get; set; }

    /// <summary>Enable XInput gamepad support. False disables controllers entirely.</summary>
    public bool UseController { get; set; } = true;

    /// <summary>Show the log console window alongside the game (useful for debugging).</summary>
    public bool KeepConsole { get; set; } = true;

    /// <summary>Enable detailed logging (game messages + save data logs) written to logs\last_run.txt.</summary>
    public bool DetailedLog { get; set; }

    /// <summary>Unlock every career track. Used for online races where tracks must be unlocked.</summary>
    public bool UnlockAllTracks { get; set; }

    /// <summary>Open any downloaded creation in the studios regardless of creator restrictions.</summary>
    public bool AllowRemix { get; set; } = true;

    /// <summary>Advanced: raw NAME=value pairs, one per line. Passed as environment variables to the game.</summary>
    public string ExtraEnvironment { get; set; } = "";

    /// <summary>UI theme for the launcher itself (see Core\Themes.cs for available themes).</summary>
    public string Theme { get; set; } = Themes.Default;

    // ==================== Graphics Settings (mapped to rsx_d3d.c on the PS3) ====================
    /// <summary>Render resolution height. Options: 720 (default), 1080, 1440, 2160. Multiplies the PS3's internal resolution.</summary>
    public int Resolution { get; set; } = 720;

    /// <summary>Anisotropic filtering quality. 0 = off, 2-16 = higher quality (performance cost increases with value).</summary>
    public int Anisotropy { get; set; }

    /// <summary>Fast approximate anti-aliasing. Smooths jagged edges at minimal performance cost.</summary>
    public bool Fxaa { get; set; }

    // ==================== Online / Creation Sync ====================
    /// <summary>Server hostname for creation upload/download (CreationSync protocol).</summary>
    public string CreationsServer { get; set; } = "";

    /// <summary>Filename of the crash log the user has already acknowledged on the Play page.</summary>
    public string CrashSeen { get; set; } = "";

    /// <summary>Latest GitHub release tag fetched at startup, shown as "What's new" on the Play page.</summary>
    public string LatestRelease { get; set; } = "";

    /// <summary>Release notes text for the LatestRelease tag.</summary>
    public string LatestNotes { get; set; } = "";

    // ==================== Save Profiles ====================
    /// <summary>Active save profile name. Empty string = game's normal saves. Non-empty = isolated profile folder.</summary>
    public string ActiveProfile { get; set; } = "";

    // ==================== Mod Management ====================
    /// <summary>List of enabled mod folder names in load order. Earlier entries take priority when two mods replace the same file.</summary>
    public List<string> EnabledMods { get; set; } = new();

    // ==================== Network / Online Settings ====================
    /// <summary>URL for the creation browser (MNR Revival's web interface).</summary>
    public string BrowserUrl { get; set; } = "https://mnrrevival.derole.net";

    /// <summary>DNS hostname that the game resolves for PSN services.</summary>
    public string ServerHost { get; set; } = "modnation.ps3.playstationcloud.com";

    /// <summary>IP/hostname the ServerHost resolves to. Default points to QuackCraft's own server.</summary>
    public string ServerAddress { get; set; } = "mnr.quackcraft.net";

    /// <summary>Settings file format version. Used for automatic migration (see Load()).</summary>
    public int SettingsVersion { get; set; }

    /// <summary>Flag: whether the game's composite cache was cleared once (set by MainWindow on first run).</summary>
    public int CompositeCacheReset { get; set; }

    /// <summary>Port of the creation server (default 10050 when 0). Used to build creation download/upload URLs.</summary>
    public int ServerPort { get; set; }

    /// <summary>Player's display name (shown in online lobbies).</summary>
    public string PlayerName { get; set; } = "";

    /// <summary>Enable online mode: signs into RPCN and routes game traffic through the configured servers.</summary>
    public bool OnlineEnabled { get; set; }

    /// <summary>UDP port for peer-to-peer race traffic. Default 3658 matches the PS3 console. Different ports needed for multiple PCs on one network.</summary>
    public int RacePort { get; set; } = 3658;

    /// <summary>RPCN ticket service host (RPCS3's RPCN implementation). Format: host:port.</summary>
    public string RpcnHost { get; set; } = "np.rpcs3.net:31313";

    /// <summary>RPCN username for online play.</summary>
    public string RpcnUser { get; set; } = "";

    /// <summary>RPCN password encrypted with Windows DPAPI (CurrentUser scope). Stored in settings.json.</summary>
    public string RpcnPasswordProtected { get; set; } = "";

    /// <summary>Email verification token required by RPCN if the account hasn't been verified yet.</summary>
    public string RpcnToken { get; set; } = "";

    // ==================== Saved Accounts ====================
    /// <summary>List of saved RPCN accounts. The active account's credentials are copied into the Rpcn* fields above.</summary>
    public List<Account> Accounts { get; set; } = new();

    /// <summary>Identifier of the currently active saved account.</summary>
    public string ActiveAccount { get; set; } = "";

    /// <summary>
    /// Unencrypted RPCN password. Backed by DPAPI encryption/decryption via Protect/Unprotect.
    /// Excluded from JSON serialization (JsonIgnore) since RpcnPasswordProtected holds the encrypted value.
    /// </summary>
    [System.Text.Json.Serialization.JsonIgnore]
    public string RpcnPassword
    {
        get => Unprotect(RpcnPasswordProtected);
        set => RpcnPasswordProtected = Protect(value);
    }

    /// <summary>
    /// Encrypts a string using Windows DPAPI (CurrentUser scope). The encrypted value can only be decrypted
    /// by the same Windows user on the same machine. Returns "" for null/empty input.
    /// </summary>
    public static string Protect(string value) => string.IsNullOrEmpty(value) ? "" :
        Convert.ToBase64String(System.Security.Cryptography.ProtectedData.Protect(System.Text.Encoding.UTF8.GetBytes(value), null, System.Security.Cryptography.DataProtectionScope.CurrentUser));

    /// <summary>
    /// Decrypts a DPAPI-encrypted base64 string. Returns "" for null/empty input or if decryption fails
    /// (e.g., the file was moved to another machine or the Windows user changed).
    /// </summary>
    public static string Unprotect(string value)
    {
        if (string.IsNullOrEmpty(value)) return "";
        try { return System.Text.Encoding.UTF8.GetString(System.Security.Cryptography.ProtectedData.Unprotect(Convert.FromBase64String(value), null, System.Security.Cryptography.DataProtectionScope.CurrentUser)); }
        catch (Exception) { return ""; }
    }

    /// <summary>
    /// Loads settings from disk. Handles automatic migration for old file formats.
    ///
    /// Migration history:
    /// - v0 → v1: Moved default server address from revival server (45.32.179.33) to QuackCraft's server.
    /// - v1 → v2: Extracted MNR_RESOLUTION, MNR_ANISO, MNR_FXAA from ExtraEnvironment into typed Settings properties.
    /// </summary>
    public static Settings Load()
    {
        try
        {
            if (File.Exists(Paths.SettingsFile))
            {
                var s = JsonSerializer.Deserialize<Settings>(File.ReadAllText(Paths.SettingsFile)) ?? new Settings();

                // Migration v0 → v1: update default server address
                if (s.SettingsVersion < 1)
                {
                    if (s.ServerAddress.Trim() == "45.32.179.33") s.ServerAddress = "mnr.quackcraft.net";
                    s.SettingsVersion = 1;
                }

                // Migration v1 → v2: convert legacy ExtraEnvironment graphics flags into typed properties
                if (s.SettingsVersion < 2)
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
        catch (Exception) { /* a corrupted settings file falls back to defaults */ }
        return new Settings();
    }

    /// <summary>
    /// Serialises settings to JSON and writes to the settings file. Creates the parent directory if needed.
    /// </summary>
    public void Save()
    {
        Directory.CreateDirectory(Paths.LauncherData);
        File.WriteAllText(Paths.SettingsFile, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
    }
}
