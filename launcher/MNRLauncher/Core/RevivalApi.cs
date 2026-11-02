using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Http;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading.Tasks;

namespace MNRLauncher.Core;

public class Counts
{
    [JsonPropertyName("all_time")] public int AllTime { get; set; }
    [JsonPropertyName("this_week")] public int ThisWeek { get; set; }
}

/// <summary>A creation as the MNR Revival API reports it.</summary>
public class RemoteCreation
{
    [JsonPropertyName("playerCreationId")] public int Id { get; set; }
    [JsonPropertyName("id")] public int AltId { set { if (Id == 0) Id = value; } get => Id; }   // the "top" lists use "id"
    [JsonPropertyName("name")] public string Name { get; set; } = "";
    [JsonPropertyName("description")] public string? Description { get; set; }
    [JsonPropertyName("rating")] public string? Rating { get; set; }
    [JsonPropertyName("creatorUsername")] public string Creator { get; set; } = "";
    [JsonPropertyName("type")] public string Type { get; set; } = "";
    [JsonPropertyName("tags")] public string? Tags { get; set; }
    [JsonPropertyName("createdAt")] public string? CreatedAt { get; set; }
    [JsonPropertyName("downloads")] public Counts? Downloads { get; set; }

    public string TypeName => Type switch { "CHARACTER" => "Character", "KART" => "Kart", "TRACK" => "Track", _ => Type };
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

public class SearchResult
{
    [JsonPropertyName("total")] public int Total { get; set; }
    [JsonPropertyName("creations")] public List<RemoteCreation> Creations { get; set; } = new();
}

/// <summary>The MNR Revival website's public API (https://mnrrevival.derole.net/api): search, top lists, details,
/// preview images, and each creation's data at /player_creations/&lt;id&gt;/data.bin.</summary>
public class RevivalApi
{
    static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(30) };
    static RevivalApi() { Http.DefaultRequestHeaders.UserAgent.ParseAdd("MNRLauncher/1.0"); }
    readonly string site;

    public RevivalApi(Settings s) { site = s.BrowserUrl.TrimEnd('/'); }

    public string PreviewUrl(int id) => $"{site}/player_creations/{id}/preview_image.png";
    public string PreviewSmallUrl(int id) => $"{site}/player_creations/{id}/preview_image_64x64.png";
    public string DataUrl(int id) => $"{site}/player_creations/{id}/data.bin";
    public string PageUrl(int id) => $"{site}/stats/creations/";

    /// <summary>type: ANY, CHARACTER (characters, which the game calls "mods"), KART or TRACK; newestFirst: the site's "desc".</summary>
    class BoardEntry
    {
        [JsonPropertyName("creationId")] public int Id { get; set; }
        [JsonPropertyName("name")] public string Name { get; set; } = "";
        [JsonPropertyName("type")] public string Type { get; set; } = "";
        [JsonPropertyName("creatorUsername")] public string Creator { get; set; } = "";
        [JsonPropertyName("downloads")] public int Downloads { get; set; }
    }
    class Board
    {
        [JsonPropertyName("total")] public int Total { get; set; }
        [JsonPropertyName("results")] public List<BoardEntry> Results { get; set; } = new();
    }

    /// <summary>Most downloaded first (the site's creation leaderboard, sortBy=downloads). It has no text search.</summary>
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

    public async Task<SearchResult> Search(string query, string type, bool newestFirst, int page, int perPage = 24)
    {
        var url = $"{site}/api/creations/search?query={Uri.EscapeDataString(query ?? "")}" + (type != "ANY" ? $"&type={type}" : "") +
                  $"&sortOrder={(newestFirst ? "desc" : "asc")}&page={page}&perPage={perPage}&platform=PS3&isMnr=true";
        return JsonSerializer.Deserialize<SearchResult>(await Http.GetStringAsync(url)) ?? new SearchResult();
    }

    /// <summary>The site's "top" list for a type (characters, karts, tracks).</summary>
    public async Task<List<RemoteCreation>> Top(string type)
    {
        var which = type switch { "CHARACTER" => "topmods", "KART" => "topkarts", _ => "toptracks" };
        return JsonSerializer.Deserialize<List<RemoteCreation>>(await Http.GetStringAsync($"{site}/api/{which}?platform=PS3")) ?? new();
    }

    public async Task<byte[]> Bytes(string url) => await Http.GetByteArrayAsync(url);

    /// <summary>Downloads a creation (data, preview, details) into MNR\launcher\data\downloads\&lt;id&gt;; returns that folder.</summary>
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
