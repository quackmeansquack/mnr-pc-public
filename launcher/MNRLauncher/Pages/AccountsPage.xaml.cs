using System;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using MNRLauncher.Core;

namespace MNRLauncher.Pages;

public partial class AccountsPage : UserControl, IRefresh
{
    bool loading;
    const string KeepSaves = "(keep the current saves)", DefaultSaves = "Default (the game's normal saves)";

    public AccountsPage() { InitializeComponent(); }

    public void Refresh()
    {
        var keep = (List.SelectedItem as Account)?.Id;
        var list = Accounts.List(MainWindow.Settings);
        List.ItemsSource = list;
        List.SelectedItem = list.FirstOrDefault(a => a.Id == keep) ?? list.FirstOrDefault(a => a.Active) ?? list.FirstOrDefault();
        Empty.Visibility = list.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        if (string.IsNullOrWhiteSpace(NewHost.Text)) NewHost.Text = RpcnClient.DefaultHost;
        ShowSelected();
    }

    void ShowSelected()
    {
        loading = true;
        var a = List.SelectedItem as Account;
        var items = new[] { KeepSaves, DefaultSaves }.Concat(Profiles.List(MainWindow.Settings).Where(p => !p.IsDefault).Select(p => p.Name)).ToList();
        SaveFor.ItemsSource = items;
        SaveFor.SelectedItem = a == null ? null : a.SaveProfile == null ? KeepSaves : a.SaveProfile.Length == 0 ? DefaultSaves : items.Contains(a.SaveProfile) ? a.SaveProfile : KeepSaves;
        SaveFor.IsEnabled = a != null;
        loading = false;
    }

    void List_SelectionChanged(object sender, SelectionChangedEventArgs e) { ShowSelected(); Status.Text = ""; }

    bool GameRunningWarning()
    {
        if (!GameRunner.IsRunning) return false;
        MessageBox.Show("Close the game first: it is signed in with the current account.", "Accounts", MessageBoxButton.OK, MessageBoxImage.Information);
        return true;
    }

    void Use_Click(object sender, RoutedEventArgs e)
    {
        if (List.SelectedItem is not Account a || GameRunningWarning()) return;
        Accounts.Use(a, MainWindow.Settings); MainWindow.Settings.Save();
        Refresh();
        Status.Text = $"Playing as {a.DisplayName}" + (a.SaveProfile == null ? "." : a.SaveProfile.Length == 0 ? ", with the default saves." : $", with the saves “{a.SaveProfile}”.");
    }

    async void Add_Click(object sender, RoutedEventArgs e)
    {
        string user = NewUser.Text.Trim(), typed = NewPassword.Password, token = NewToken.Text.Trim(), host = NewHost.Text.Trim();
        if (user.Length == 0 || typed.Length == 0) { AddStatus.Text = "Enter the user name and password."; return; }
        AddStatus.Text = "Preparing the password...";
        var derived = await Task.Run(() => RpcnClient.DerivePassword(typed));   // stored the way RPCS3 stores it, never the typed password
        var s = MainWindow.Settings;
        bool first = s.Accounts.Count == 0;
        var a = Accounts.AddOrUpdate(s, user, derived, token, host);
        if (first && !GameRunner.IsRunning) Accounts.Use(a, s);
        s.Save();
        NewUser.Text = ""; NewPassword.Password = ""; NewToken.Text = "";
        AddStatus.Text = $"Saved {a.DisplayName}." + (first ? " It's the account in use." : " Select it and press “Use this account” to switch.");
        Refresh();
        List.SelectedItem = (List.ItemsSource as System.Collections.Generic.IEnumerable<Account>)?.FirstOrDefault(x => x.Id == a.Id);
    }

    /// <summary>A new RPCN account from the fields (Create), saved to the list; RPCN e-mails its token.</summary>
    async void Create_Click(object sender, RoutedEventArgs e)
    {
        string user = NewUser.Text.Trim(), typed = NewPassword.Password, email = NewEmail.Text.Trim(), host = NewHost.Text.Trim();
        if (!RpcnClient.ValidUserName(user)) { AddStatus.Text = "User names are 3 to 16 letters, digits, - or _."; return; }
        if (typed.Length == 0) { AddStatus.Text = "Type the new account's password."; return; }
        if (!RpcnClient.ValidEmail(email)) { AddStatus.Text = "Enter a valid e-mail address: RPCN e-mails the token there."; return; }
        if (MessageBox.Show($"Create this RPCN account?\n\nUser name: {user}\nE-mail: {email}\n\nRPCN e-mails a token to that address (it is only used for that and password resets). Remember the password: it can't be shown again.",
                "Create RPCN account", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes) return;
        AddStatus.Text = "Creating the account...";
        try
        {
            var derived = await Task.Run(() =>
            {
                var d = RpcnClient.DerivePassword(typed);
                using var c = RpcnClient.Connect(host.Length > 0 ? host : RpcnClient.DefaultHost);
                c.CreateAccount(user, d, email);
                return d;
            });
            var s = MainWindow.Settings;
            var a = Accounts.AddOrUpdate(s, user, derived, "", host);
            s.Save();
            NewToken.Text = ""; NewEmail.Text = "";
            AddStatus.Text = $"Created {user} and added it below. RPCN e-mailed a token to {email}: put it in the token field (user name and password stay filled in) and press Add account, then Use this account.";
            Refresh();
            List.SelectedItem = (List.ItemsSource as System.Collections.Generic.IEnumerable<Account>)?.FirstOrDefault(x => x.Id == a.Id);
        }
        catch (Exception ex) { AddStatus.Text = ex.Message; }
    }

    /// <summary>E-mails the selected account's token again.</summary>
    async void Resend_Click(object sender, RoutedEventArgs e)
    {
        if (List.SelectedItem is not Account a) { Status.Text = "Select an account first."; return; }
        Status.Text = "Asking RPCN to resend the token...";
        try
        {
            await Task.Run(() =>
            {
                using var c = RpcnClient.Connect(string.IsNullOrWhiteSpace(a.RpcnHost) ? RpcnClient.DefaultHost : a.RpcnHost);
                c.ResendToken(a.RpcnUser, Accounts.Password(a));
            });
            Status.Text = "RPCN e-mailed the token again (check the spam folder too).";
        }
        catch (Exception ex) { Status.Text = ex.Message; }
    }

    void Import_Click(object sender, RoutedEventArgs e)
    {
        var file = Accounts.FindRpcs3Config();
        if (file == null)
        {
            var dlg = new Microsoft.Win32.OpenFileDialog { Title = "RPCS3's rpcn.yml (in RPCS3's config folder)", Filter = "rpcn.yml|rpcn.yml|YAML|*.yml" };
            if (dlg.ShowDialog() != true) return;
            file = dlg.FileName;
        }
        var (npid, pass, token, host) = Accounts.ReadRpcs3Config(file);
        if (npid.Length == 0 || pass.Length == 0) { AddStatus.Text = $"No account found in {file}."; return; }
        var s = MainWindow.Settings;
        bool first = s.Accounts.Count == 0;
        var a = Accounts.AddOrUpdate(s, npid, pass, token, host);   // RPCS3 already stores the derived password
        if (first && !GameRunner.IsRunning) Accounts.Use(a, s);
        s.Save();
        AddStatus.Text = $"Imported {npid} from RPCS3.";
        Refresh();
    }

    async void Test_Click(object sender, RoutedEventArgs e)
    {
        if (List.SelectedItem is not Account a) return;
        Status.Text = "Signing in...";
        try
        {
            var (name, ticket) = await Task.Run(() =>
            {
                var pass = Accounts.Password(a);
                if (a.RpcnUser.Length == 0 || pass.Length == 0) throw new IOException("This account has no password saved: add it again with the password.");
                using var c = RpcnClient.Connect(string.IsNullOrWhiteSpace(a.RpcnHost) ? RpcnClient.DefaultHost : a.RpcnHost);
                c.Login(a.RpcnUser, pass, a.RpcnToken);
                return (c.OnlineName, c.RequestTicket("UP9000-BCUS98167_00", Array.Empty<byte>()));
            });
            Status.Text = $"Signed in as {name}; got a {ticket.Length}-byte ticket for ModNation Racers.";
        }
        catch (Exception ex) { Status.Text = ex.Message; }
    }

    void SaveFor_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (loading || List.SelectedItem is not Account a || SaveFor.SelectedItem is not string choice) return;
        a.SaveProfile = choice == KeepSaves ? null : choice == DefaultSaves ? "" : choice;
        var s = MainWindow.Settings;
        if (a.Id == s.ActiveAccount && a.SaveProfile != null && !GameRunner.IsRunning) s.ActiveProfile = a.SaveProfile;   // the account in use switches now
        s.Save();
        var id = a.Id; Refresh(); List.SelectedItem = (List.ItemsSource as System.Collections.Generic.IEnumerable<Account>)?.FirstOrDefault(x => x.Id == id);
    }

    void Remove_Click(object sender, RoutedEventArgs e)
    {
        if (List.SelectedItem is not Account a || GameRunningWarning()) return;
        if (MessageBox.Show($"Remove the account “{a.DisplayName}” from the launcher? (The RPCN account itself and any saves stay.)", "Remove account",
                MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes) return;
        Accounts.Remove(a, MainWindow.Settings); MainWindow.Settings.Save();
        Refresh();
    }
}
