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
    [JsonIgnore] public string DisplayName => RpcnUser.Length > 0 ? RpcnUser : "(no user name)";
    [JsonIgnore] public string Badge => Active ? "IN USE" : "";
    public override string ToString() => DisplayName;   // the dark ComboBox template shows the closed box's item as text
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
    /// <summary>Settings from before accounts existed: the one configured sign-in becomes the first account.</summary>
    public static void Migrate(Settings s)
    {
        if (s.Accounts.Count > 0 || string.IsNullOrWhiteSpace(s.RpcnUser)) return;
        var a = FromSettings(s, new Account());
        s.Accounts.Add(a); s.ActiveAccount = a.Id;
    }

    public static List<Account> List(Settings s)
    {
        Migrate(s);
        foreach (var a in s.Accounts) a.Active = a.Id == s.ActiveAccount;
        return s.Accounts.OrderBy(a => a.RpcnUser, StringComparer.OrdinalIgnoreCase).ToList();
    }

    public static Account? Active(Settings s) => List(s).FirstOrDefault(a => a.Active);

    /// <summary>Make <paramref name="a"/> the account the game signs in with (and switch to its saves, if it has some).</summary>
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

    /// <summary>After the Settings page changed the sign-in fields: keep the account in use up to date.</summary>
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

    /// <summary>Adds an account (or updates the one with the same user name and server); returns it.</summary>
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

    public static void Remove(Account a, Settings s)
    {
        s.Accounts.RemoveAll(x => x.Id == a.Id);
        if (s.ActiveAccount != a.Id) return;
        s.ActiveAccount = "";
        var next = s.Accounts.FirstOrDefault();
        if (next != null) Use(next, s);
        else { s.RpcnUser = ""; s.RpcnPasswordProtected = ""; s.RpcnToken = ""; }
    }

    public static string Password(Account a) => Settings.Unprotect(a.RpcnPasswordProtected);

    /// <summary>The author written into exported creations and new mods: the account in use (a name chosen with an
    /// earlier launcher, else its RPCN name), else the Windows user.</summary>
    public static string ExportName(Settings s)
    {
        var a = s.Accounts.FirstOrDefault(x => x.Id == s.ActiveAccount);
        if (a != null) return a.PlayerName.Length > 0 ? a.PlayerName : a.RpcnUser;
        return s.PlayerName.Trim().Length > 0 ? s.PlayerName.Trim() : Environment.UserName;
    }

    /// <summary>RPCS3 keeps its account in config\rpcn.yml (NPID, derived password, token, host). Returns the file read, or null.</summary>
    public static string? FindRpcs3Config()
    {
        var candidates = new[]
        {
            Path.Combine(Paths.Root, "tools", "rpcs3", "config", "rpcn.yml"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "rpcs3", "config", "rpcn.yml"),
        };
        return candidates.FirstOrDefault(File.Exists);
    }

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
