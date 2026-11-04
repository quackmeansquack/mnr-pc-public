using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using MNRLauncher.Core;

namespace MNRLauncher.Pages;

public partial class BrowsePage : UserControl, IRefresh
{
    /// <summary>A row in the creations list: a RemoteCreation with an optional thumbnail and display properties.</summary>
    public class Row
    {
        public RemoteCreation C { get; init; } = new();
        public ImageSource? Thumb { get; init; }
        public bool IsTrack => C.Type == "TRACK";
        public Stretch Fit => IsTrack ? Stretch.UniformToFill : Stretch.Uniform;   // track pictures are screenshots; the rest stand on the stage
        public Thickness PictureMargin => IsTrack ? new Thickness(0) : new Thickness(10, 8, 10, 4);
        public string Line2 => $"{C.TypeName} · by {C.Creator}";
        public string Line3
        {
            get
            {
                var bits = new List<string>();
                if (!string.IsNullOrEmpty(C.Rating) && C.Rating != "0.0") bits.Add("★ " + C.Rating);
                if (C.Downloads != null) bits.Add($"{C.Downloads.AllTime:N0} download{(C.Downloads.AllTime == 1 ? "" : "s")}");
                if (C.CreatedAt is { Length: >= 10 }) bits.Add(C.CreatedAt[..10]);
                return string.Join("  ·  ", bits);
            }
        }
        public Visibility InstalledTag => InstalledCache.Contains(C.Id) ? Visibility.Visible : Visibility.Collapsed;
    }
    /// <summary>The page's cached set of creation IDs that are currently installed in the game.</summary>
    static HashSet<int> InstalledCache = new();

    RevivalApi api = new(MainWindow.Settings);
    int page = 1, total;
    const int PerPage = 24;
    bool loaded;

    public BrowsePage() { InitializeComponent(); }

    /// <summary>Refreshes the browse page: creates a new API client, updates the source description, and loads the listings asynchronously if not already loaded.</summary>
    public void Refresh()
    {
        api = new RevivalApi(MainWindow.Settings);
        Source.Text = $"Creations shared on {new Uri(MainWindow.Settings.BrowserUrl).Host} (MNR Revival). “Download to game” downloads one the way the game does, from the server you play on (which copies it from MNR Revival first), credited to its creator.";
        if (!loaded) { loaded = true; _ = Load(); }
    }

    string Type => TypeBox.SelectedIndex switch { 1 => "CHARACTER", 2 => "KART", 3 => "TRACK", _ => "ANY" };

    /// <summary>Loads the creations listing: searches or fetches downloads based on the user's filters, checks installation status, and populates the UI.</summary>
    async Task Load()
    {
        Message.Text = "Loading…"; List.ItemsSource = null; Prev.IsEnabled = Next.IsEnabled = false;
        try
        {
            var q = Query.Text.Trim();
            SearchResult r;
            if (SortBox.SelectedIndex == 2)
            {   // the downloads leaderboard has no text search: filter this page by name
                r = await api.MostDownloaded(Type, page, PerPage);
                if (q.Length > 0) r.Creations = r.Creations.Where(c => c.Name.Contains(q, StringComparison.OrdinalIgnoreCase) || c.Creator.Contains(q, StringComparison.OrdinalIgnoreCase)).ToList();
            }
            else r = await api.Search(q, Type, SortBox.SelectedIndex != 1, page, PerPage);
            total = r.Total;
            InstalledCache = r.Creations.Where(c => { try { return CreationDownload.Installed(c.Id) != null; } catch (Exception) { return false; } }).Select(c => c.Id).ToHashSet();
            List.ItemsSource = r.Creations.Select(c => new Row { C = c, Thumb = Image(api.PreviewUrl(c.Id)) }).ToList();
            int pages = Math.Max(1, (total + PerPage - 1) / PerPage);
            PageInfo.Text = total == 0 ? "" : $"{total:N0} creations  ·  page {page} of {pages:N0}";
            Message.Text = r.Creations.Count == 0 ? "Nothing found." : "";
            Prev.IsEnabled = page > 1; Next.IsEnabled = page < pages;
        }
        catch (Exception ex) { Message.Text = "Couldn't reach the server:\n" + ex.Message; }
    }

    /// <summary>Loads an image from a URL with caching and decode pixel limit.</summary>
    static ImageSource? Image(string url)
    {
        try
        {
            var b = new BitmapImage();
            b.BeginInit(); b.UriSource = new Uri(url); b.CacheOption = BitmapCacheOption.OnLoad; b.DecodePixelWidth = 256; b.EndInit();
            return b;
        }
        catch (Exception) { return null; }
    }

    /// <summary>As many tile columns as fit (tiles about 200 px wide and stretched to fill the row).</summary>
    public static readonly DependencyProperty ColumnsProperty = DependencyProperty.Register(nameof(Columns), typeof(int), typeof(BrowsePage), new PropertyMetadata(3));
    public int Columns { get => (int)GetValue(ColumnsProperty); set => SetValue(ColumnsProperty, value); }
    void List_SizeChanged(object sender, SizeChangedEventArgs e) => Columns = Math.Max(1, (int)((e.NewSize.Width - 20) / 200));

    /// <summary>Triggers a search with the current query and filters, resetting to page 1.</summary>
    void Search_Click(object sender, RoutedEventArgs e) { page = 1; _ = Load(); }
    /// <summary>Handles Enter key press in the query box to trigger a search.</summary>
    void Query_KeyDown(object sender, System.Windows.Input.KeyEventArgs e) { if (e.Key == System.Windows.Input.Key.Enter) { page = 1; _ = Load(); } }
    /// <summary>Re-loads the listing when sort or type filters change.</summary>
    void Filter_Changed(object sender, SelectionChangedEventArgs e) { if (loaded && IsLoaded) { page = 1; _ = Load(); } }
    /// <summary>Navigates to the previous page of results.</summary>
    void Prev_Click(object sender, RoutedEventArgs e) { if (page > 1) { page--; _ = Load(); } }
    /// <summary>Navigates to the next page of results.</summary>
    void Next_Click(object sender, RoutedEventArgs e) { page++; _ = Load(); }

    /// <summary>Shows the detail card for the selected creation: loads preview image, name, summary, description, tags, and installation status.</summary>
    void List_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (List.SelectedItem is not Row r) { DetailCard.Visibility = Visibility.Collapsed; return; }
        var c = r.C;
        DetailCard.Visibility = Visibility.Visible;
        DetailImage.Source = Image(api.PreviewUrl(c.Id));
        DetailName.Text = c.Name;
        DetailSummary.Text = c.Summary;
        DetailDesc.Text = c.Description ?? "";
        DetailTags.Text = string.IsNullOrEmpty(c.Tags) ? "" : "Tags: " + c.Tags.Replace(",", ", ");
        ShowInstalled(c);
    }

    /// <summary>Displays the installation status of a creation and updates the download button accordingly.</summary>
    void ShowInstalled(RemoteCreation c)
    {
        string? stem = null;
        try { stem = CreationDownload.Installed(c.Id); } catch (Exception) { }
        DetailStatus.Text = stem != null ? $"In the game, with your downloads ({Path.GetFileName(stem)[9..]})" : "";
        DownloadButton.Content = stem != null ? "Download again" : "Download to game";
        DownloadButton.IsEnabled = !GameRunner.IsRunning;
        RemoveButton.Visibility = stem != null ? Visibility.Visible : Visibility.Collapsed;
        if (GameRunner.IsRunning) DetailStatus.Text += (DetailStatus.Text.Length > 0 ? "\n" : "") + "Close the game to download: it reads its creations when it starts.";
    }

    /// <summary>Downloads it as the game would from the server it plays on (CreationDownload): that server copies it from
    /// MNR Revival first if it can, and the creation keeps its Revival id and creator.</summary>
    async void Download_Click(object sender, RoutedEventArgs e)
    {
        if (List.SelectedItem is not Row r) return;
        DownloadButton.IsEnabled = false; DetailStatus.Text = "Downloading…";
        try
        {
            await CreationDownload.Install(MainWindow.Settings, r.C.Id, t => Dispatcher.Invoke(() => DetailStatus.Text = t));
            InstalledCache.Add(r.C.Id);
            ShowInstalled(r.C);
            DetailStatus.Text = $"Downloaded. It's with your downloaded {r.C.TypeName.ToLowerInvariant()}s the next time the game starts, credited to {r.C.Creator}.";
        }
        catch (Exception ex) { DetailStatus.Text = "Download failed: " + ex.Message; DownloadButton.IsEnabled = true; }
    }

    /// <summary>Removes the selected creation from the game's local installations.</summary>
    void Remove_Click(object sender, RoutedEventArgs e)
    {
        if (List.SelectedItem is not Row r) return;
        try { CreationDownload.Remove(r.C.Id); InstalledCache.Remove(r.C.Id); ShowInstalled(r.C); DetailStatus.Text = "Removed from the game."; }
        catch (Exception ex) { DetailStatus.Text = "Couldn't remove it: " + ex.Message; }
    }
}
