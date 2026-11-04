using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json.Serialization;

namespace MNRLauncher.Core;

/// <summary>A saved RPCN sign-in (the online identity the game plays as), optionally tied to a save profile.</summary>
public class Account
{
    public string Id { get; set; } = Guid.NewGuid().ToString("N");
    public string RpcnUser { get; set; } = "";
    public string RpcnPasswordProtected { get; set; } = "";   // derived password, DPAPI-encrypted (as in Settings)
    public string RpcnToken { get; set; } = "";
    public string RpcnHost { get; set; } = RpcnClient.DefaultHost;
    public string PlayerName { get; set; } = "";              // name written into exported creations ("" = the RPCN name)
    public string? SaveProfile { get; set; }                  // save profile switched to with the account (null: leave as is, "": default saves)

    [JsonIgnore] public bool Active { get; set; }
    /// <summary>The display name shown in the accounts ComboBox.</summary>
    [JsonIgnore] public string DisplayName => RpcnUser.Length > 0 ? RpcnUser : "(no user name)";
    /// <summary>Badge text for the active account in the UI.</summary>
    [JsonIgnore] public string Badge => Active ? "IN USE" : "";
    /// <summary>String representation for the ComboBox template.</summary>
    public override string ToString() => DisplayName;   // the dark ComboBox template shows the closed box's item as text
    /// <summary>A summary line for the account details (password status, save profile, custom host).</summary>
    [JsonIgnore]
    public string Summary
    {
        get
        {
            var bits = new List<string>();
            bits.Add(string.IsNullOrEmpty(RpcnPasswordProtected) ? "no password saved" : "password saved");
            bits.Add(SaveProfile == null ? "keeps the current saves" : SaveProfile.Length == 0 ? "default saves" : "saves: " + SaveProfile);
            if (!string.IsNullOrEmpty(RpcnHost) && RpcnHost != RpcnClient.DefaultHost) bits.Add(RpcnHost);
            return string.Join("  ·  ", bits);
        }
    }
}

/// <summary>Several RPCN accounts and switching between them. The account in use is copied into the Settings fields the
/// game runner reads (RpcnUser, password, token, host, player name), and its linked save profile becomes the active one.</summary>
public static class Accounts
{
    /// <summary>
    /// Migrates legacy single-account settings (pre-multi-account) into the Accounts system.
    /// Creates the first account from the old Settings fields if no accounts exist yet.
    /// </summary>
    public static void Migrate(Settings s)
    {
        if (s.Accounts.Count > 0 || string.IsNullOrWhiteSpace(s.RpcnUser)) return;
        var a = FromSettings(s, new Account());
        s.Accounts.Add(a); s.ActiveAccount = a.Id;
    }

    /// <summary>Returns the full account list, marking the active account. Triggers migration if needed.</summary>
    public static List<Account> List(Settings s)
    {
        Migrate(s);
        foreach (var a in s.Accounts) a.Active = a.Id == s.ActiveAccount;
        return s.Accounts.OrderBy(a => a.RpcnUser, StringComparer.OrdinalIgnoreCase).ToList();
    }

    /// <summary>Returns the currently active account, or null if none is set.</summary>
    public static Account? Active(Settings s) => List(s).FirstOrDefault(a => a.Active);

    /// <summary>
    /// Makes <paramref name="a"/> the account the game signs in with and switches to its linked save profile.
    /// Copies account credentials into Settings fields that the game runner reads.
    /// Creates a dedicated save profile for non-first accounts if one hasn't been assigned.
    /// </summary>
    public static void Use(Account a, Settings s)
    {
        /* Saves per account: the game ties a save to the console identity it was made under, and each account signs in
         * as its own (host.c, the PSID per account): sharing one save, a second account lost the first one's racer and
         * left the save looking like another console's. The first account keeps the normal saves; any other gets a save
         * profile of its own (named after it) unless one was chosen on the Accounts page. */
        if (a.SaveProfile == null)
        {
            if (s.Accounts.Count == 0 || s.Accounts[0].Id == a.Id) a.SaveProfile = "";
            else
            {
                var folder = ModManager.SafeName(a.RpcnUser.Length > 0 ? a.RpcnUser : "Account");
                try { if (!Directory.Exists(Path.Combine(Profiles.Dir, folder))) Profiles.Create(folder, null); a.SaveProfile = folder; }
                catch (Exception) { /* keep the saves in use */ }
            }
        }
        s.ActiveAccount = a.Id;
        s.RpcnUser = a.RpcnUser; s.RpcnPasswordProtected = a.RpcnPasswordProtected; s.RpcnToken = a.RpcnToken;
        s.RpcnHost = string.IsNullOrWhiteSpace(a.RpcnHost) ? RpcnClient.DefaultHost : a.RpcnHost;
        s.PlayerName = a.PlayerName.Length > 0 ? a.PlayerName : a.RpcnUser;
        if (a.SaveProfile != null && (a.SaveProfile.Length == 0 || Directory.Exists(Path.Combine(Profiles.Dir, a.SaveProfile))))
            s.ActiveProfile = a.SaveProfile;
    }

    /// <summary>
    /// Syncs the active account's fields after the user edits sign-in data on the Settings page.
    /// Creates a new account entry if the active account doesn't exist yet.
    /// </summary>
    public static void SyncActive(Settings s)
    {
        Migrate(s);
        var a = s.Accounts.FirstOrDefault(x => x.Id == s.ActiveAccount);
        if (a == null)
        {
            if (string.IsNullOrWhiteSpace(s.RpcnUser)) return;
            a = s.Accounts.FirstOrDefault(x => string.Equals(x.RpcnUser, s.RpcnUser.Trim(), StringComparison.OrdinalIgnoreCase));
            if (a == null) { a = new Account(); s.Accounts.Add(a); }
            s.ActiveAccount = a.Id;
        }
        FromSettings(s, a);
    }

    static Account FromSettings(Settings s, Account a)
    {
        a.RpcnUser = s.RpcnUser.Trim(); a.RpcnPasswordProtected = s.RpcnPasswordProtected; a.RpcnToken = s.RpcnToken.Trim();
        a.RpcnHost = string.IsNullOrWhiteSpace(s.RpcnHost) ? RpcnClient.DefaultHost : s.RpcnHost.Trim();
        a.PlayerName = s.PlayerName.Trim() == a.RpcnUser ? "" : s.PlayerName.Trim();
        return a;
    }

    /// <summary>
    /// Adds a new account or updates an existing one with the same RPCN username and server host.
    /// Encrypts the password with DPAPI if provided. Automatically applies the account if it's the active one.
    /// </summary>
    public static Account AddOrUpdate(Settings s, string user, string derivedPassword, string token, string host)
    {
        Migrate(s);
        host = string.IsNullOrWhiteSpace(host) ? RpcnClient.DefaultHost : host.Trim();
        var a = s.Accounts.FirstOrDefault(x => string.Equals(x.RpcnUser, user, StringComparison.OrdinalIgnoreCase) && string.Equals(x.RpcnHost, host, StringComparison.OrdinalIgnoreCase));
        if (a == null) { a = new Account { RpcnUser = user.Trim(), RpcnHost = host }; s.Accounts.Add(a); }
        if (derivedPassword.Length > 0) a.RpcnPasswordProtected = Settings.Protect(derivedPassword);
        a.RpcnToken = token.Trim();
        if (a.Id == s.ActiveAccount) Use(a, s);   // editing the account in use: the game gets the new details too
        return a;
    }

    /// <summary>Removes the account. If it was active, activates the next available account or clears sign-in fields.</summary>
    public static void Remove(Account a, Settings s)
    {
        s.Accounts.RemoveAll(x => x.Id == a.Id);
        if (s.ActiveAccount != a.Id) return;
        s.ActiveAccount = "";
        var next = s.Accounts.FirstOrDefault();
        if (next != null) Use(next, s);
        else { s.RpcnUser = ""; s.RpcnPasswordProtected = ""; s.RpcnToken = ""; }
    }

    /// <summary>Decrypts and returns the account's DPAPI-protected password.</summary>
    public static string Password(Account a) => Settings.Unprotect(a.RpcnPasswordProtected);

    /// <summary>
    /// Returns the display name used for exported creations and new mods.
    /// Prefers the account's custom player name, then RPCN name, then the legacy PlayerName setting, then the Windows username.
    /// </summary>
    public static string ExportName(Settings s)
    {
        var a = s.Accounts.FirstOrDefault(x => x.Id == s.ActiveAccount);
        if (a != null) return a.PlayerName.Length > 0 ? a.PlayerName : a.RpcnUser;
        return s.PlayerName.Trim().Length > 0 ? s.PlayerName.Trim() : Environment.UserName;
    }

    /// <summary>
    /// Locates the RPCS3 RPCN configuration file (rpcn.yml) from either the launcher's tools directory or the user's AppData.
    /// Returns the file path if found, or null if neither location contains the config.
    /// </summary>
    public static string? FindRpcs3Config()
    {
        var candidates = new[]
        {
            Path.Combine(Paths.Root, "tools", "rpcs3", "config", "rpcn.yml"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "rpcs3", "config", "rpcn.yml"),
        };
        return candidates.FirstOrDefault(File.Exists);
    }

    /// <summary>
    /// Parses the RPCS3 rpcn.yml file to extract NPID, derived password, token, and server host.
    /// Validates the host field and falls back to the default RPCN host if invalid.
    /// </summary>
    public static (string npid, string password, string token, string host) ReadRpcs3Config(string file)
    {
        string Field(string name)
        {
            foreach (var raw in File.ReadAllLines(file))
            {
                var line = raw.Trim();
                if (line.StartsWith(name + ":", StringComparison.OrdinalIgnoreCase)) return line[(name.Length + 1)..].Trim().Trim('"', '\'');
            }
            return "";
        }
        var host = Field("Host");
        if (host.Length == 0 || host.StartsWith("Official", StringComparison.OrdinalIgnoreCase) || !host.Contains('.')) host = RpcnClient.DefaultHost;
        return (Field("NPID"), Field("Password"), Field("Token"), host);
    }
}
