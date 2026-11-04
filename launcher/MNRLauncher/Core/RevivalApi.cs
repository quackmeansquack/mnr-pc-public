using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Http;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading.Tasks;

namespace MNRLauncher.Core;

/// <summary>Download count statistics broken down by time period, as returned by the MNR Revival API.</summary>
public class Counts
{
    [JsonPropertyName("all_time")] public int AllTime { get; set; }
    [JsonPropertyName("this_week")] public int ThisWeek { get; set; }
}

/// <summary>A creation as the MNR Revival API reports it.</summary>
public class RemoteCreation
{
    [JsonPropertyName("playerCreationId")] public int Id { get; set; }
    /// <summary>Alternative ID used by the site's "top" lists (topmods/topkarts/toptracks). Merged into Id if Id is 0.</summary>
    [JsonPropertyName("id")] public int AltId { set { if (Id == 0) Id = value; } get => Id; }
    /// <summary>The creation's title as displayed on the site.</summary>
    [JsonPropertyName("name")] public string Name { get; set; } = "";
    /// <summary>The creation's description text.</summary>
    [JsonPropertyName("description")] public string? Description { get; set; }
    /// <summary>Star rating string (e.g. "4.5") from the site.</summary>
    [JsonPropertyName("rating")] public string? Rating { get; set; }
    /// <summary>The username of the creation's creator.</summary>
    [JsonPropertyName("creatorUsername")] public string Creator { get; set; } = "";
    /// <summary>The creation type: "CHARACTER", "KART", or "TRACK".</summary>
    [JsonPropertyName("type")] public string Type { get; set; } = "";
    /// <summary>Comma-separated tags associated with the creation.</summary>
    [JsonPropertyName("tags")] public string? Tags { get; set; }
    /// <summary>ISO 8601 creation timestamp from the site.</summary>
    [JsonPropertyName("createdAt")] public string? CreatedAt { get; set; }
    /// <summary>Download count statistics for this creation.</summary>
    [JsonPropertyName("downloads")] public Counts? Downloads { get; set; }

    /// <summary>Human-readable type name: "CHARACTER" → "Character", "KART" → "Kart", "TRACK" → "Track".</summary>
    public string TypeName => Type switch { "CHARACTER" => "Character", "KART" => "Kart", "TRACK" => "Track", _ => Type };
    /// <summary>Formatted summary string combining type, creator, rating, download count, and date.</summary>
    public string Summary
    {
        get
        {
            var parts = new List<string> { TypeName, "by " + Creator };
            if (!string.IsNullOrEmpty(Rating) && Rating != "0.0") parts.Add("★ " + Rating);
            if (Downloads != null) parts.Add($"{Downloads.AllTime} downloads");
            if (CreatedAt is { Length: >= 10 }) parts.Add(CreatedAt[..10]);
            return string.Join("  ·  ", parts);
        }
    }
}

/// <summary>Search result wrapper containing the total count and matching creations list.</summary>
public class SearchResult
{
    [JsonPropertyName("total")] public int Total { get; set; }
    [JsonPropertyName("creations")] public List<RemoteCreation> Creations { get; set; } = new();
}

/// <summary>The MNR Revival website's public API (https://mnrrevival.derole.net/api): search, top lists, details,
/// preview images, and each creation's data at /player_creations/&lt;id&gt;/data.bin.</summary>
public class RevivalApi
{
    /// <summary>Shared HTTP client with 30-second timeout and launcher User-Agent header.</summary>
    static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(30) };
    static RevivalApi() { Http.DefaultRequestHeaders.UserAgent.ParseAdd("MNRLauncher/1.0"); }
    /// <summary>The MNR Revival website base URL (e.g. "https://mnrrevival.derole.net").</summary>
    readonly string site;

    /// <summary>Initialises the API client with the site URL from <paramref name="s"/>.</summary>
    public RevivalApi(Settings s) { site = s.BrowserUrl.TrimEnd('/'); }

    /// <summary>Returns the URL for a creation's full-size preview image.</summary>
    public string PreviewUrl(int id) => $"{site}/player_creations/{id}/preview_image.png";
    /// <summary>Returns the URL for a creation's 64×64 thumbnail preview.</summary>
    public string PreviewSmallUrl(int id) => $"{site}/player_creations/{id}/preview_image_64x64.png";
    /// <summary>Returns the URL for a creation's raw data file (data.bin).</summary>
    public string DataUrl(int id) => $"{site}/player_creations/{id}/data.bin";
    /// <summary>Returns the URL for a creation's stats page on the site.</summary>
    public string PageUrl(int id) => $"{site}/stats/creations/";

    /// <summary>
    /// Internal model for the site's leaderboard API response (creationId, name, type, creator, downloads).
    /// </summary>
    class BoardEntry
    {
        [JsonPropertyName("creationId")] public int Id { get; set; }
        [JsonPropertyName("name")] public string Name { get; set; } = "";
        [JsonPropertyName("type")] public string Type { get; set; } = "";
        [JsonPropertyName("creatorUsername")] public string Creator { get; set; } = "";
        [JsonPropertyName("downloads")] public int Downloads { get; set; }
    }
    /// <summary>Internal model for the site's leaderboard API response wrapper.</summary>
    class Board
    {
        [JsonPropertyName("total")] public int Total { get; set; }
        [JsonPropertyName("results")] public List<BoardEntry> Results { get; set; } = new();
    }

    /// <summary>
    /// Fetches the site's creation leaderboard sorted by most downloads.
    /// <paramref name="type"/> filters by "CHARACTER", "KART", "TRACK", or "ANY".
    /// </summary>
    public async Task<SearchResult> MostDownloaded(string type, int page, int perPage = 24)
    {
        var url = $"{site}/api/leaderboards/creations?" + (type != "ANY" ? $"type={type}&" : "") + $"sortBy=downloads&page={page}&perPage={perPage}&platform=PS3";
        var b = JsonSerializer.Deserialize<Board>(await Http.GetStringAsync(url)) ?? new Board();
        return new SearchResult
        {
            Total = b.Total,
            Creations = b.Results.Select(e => new RemoteCreation
            {
                Id = e.Id, Name = e.Name, Type = e.Type, Creator = e.Creator, Downloads = new Counts { AllTime = e.Downloads },
            }).ToList(),
        };
    }

    /// <summary>
    /// Searches creations by text query, optionally filtered by type and sort order.
    /// <paramref name="newestFirst"/> controls ascending/descending date sort.
    /// </summary>
    public async Task<SearchResult> Search(string query, string type, bool newestFirst, int page, int perPage = 24)
    {
        var url = $"{site}/api/creations/search?query={Uri.EscapeDataString(query ?? "")}" + (type != "ANY" ? $"&type={type}" : "") +
                  $"&sortOrder={(newestFirst ? "desc" : "asc")}&page={page}&perPage={perPage}&platform=PS3&isMnr=true";
        return JsonSerializer.Deserialize<SearchResult>(await Http.GetStringAsync(url)) ?? new SearchResult();
    }

    /// <summary>
    /// Fetches the site's curated "top" list for a type: "CHARACTER" → topmods, "KART" → topkarts, else → toptracks.
    /// </summary>
    public async Task<List<RemoteCreation>> Top(string type)
    {
        var which = type switch { "CHARACTER" => "topmods", "KART" => "topkarts", _ => "toptracks" };
        return JsonSerializer.Deserialize<List<RemoteCreation>>(await Http.GetStringAsync($"{site}/api/{which}?platform=PS3")) ?? new();
    }

    /// <summary>Downloads raw bytes from the given URL using the shared HttpClient.</summary>
    public async Task<byte[]> Bytes(string url) => await Http.GetByteArrayAsync(url);

    /// <summary>
    /// Downloads a creation's data.bin, preview images, and creation.json into a named folder under DownloadsDir.
    /// Returns the path to the created folder.
    /// </summary>
    public async Task<string> Download(RemoteCreation c)
    {
        var dir = Path.Combine(Paths.DownloadsDir, $"{c.Id}_{ModManager.SafeName(c.Name)}");
        Directory.CreateDirectory(dir);
        await File.WriteAllBytesAsync(Path.Combine(dir, "data.bin"), await Bytes(DataUrl(c.Id)));
        try { await File.WriteAllBytesAsync(Path.Combine(dir, "preview.png"), await Bytes(PreviewUrl(c.Id))); } catch (HttpRequestException) { }
        try { await File.WriteAllBytesAsync(Path.Combine(dir, "preview_small.png"), await Bytes(PreviewSmallUrl(c.Id))); } catch (HttpRequestException) { }
        await File.WriteAllTextAsync(Path.Combine(dir, "creation.json"), JsonSerializer.Serialize(c, new JsonSerializerOptions { WriteIndented = true }));
        return dir;
    }
}
