using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Xml.Linq;
using GameServer.Models;
using GameServer.Models.PlayerData;
using GameServer.Models.PlayerData.PlayerCreations;
using GameServer.Utils;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc;
using Microsoft.Extensions.DependencyInjection;

[assembly: HostingStartup(typeof(GameServer.Controllers.MnrPc.MnrPcStartup))]

namespace GameServer.Controllers.MnrPc
{
    /// <summary>Creations from the MNR Revival server, mirrored here under the same id so a creation downloaded from
    /// Revival is valid on this server too (the game checks every published creation it holds against the server it plays
    /// on, and a stock PLGarage answers "ban" for ids it doesn't know). The server fetches the creation and its record from
    /// Revival itself; clients only name an id, so nothing uploaded by a client is stored. Each creator gets an account here
    /// named after their Revival username (no sign-in attached), which becomes theirs if they ever sign in with that name.
    /// A creation's parent (the one it remixes) comes along, as on Revival. Ids for this server's own new creations start
    /// far above Revival's (ALTER TABLE PlayerCreations AUTO_INCREMENT = 1000000): ids 10000-999999 are Revival's.</summary>
    public static class MnrPcImport
    {
        public const int First = 10000, OwnFrom = 1000000;
        static readonly string Revival = (Environment.GetEnvironmentVariable("MNRPC_REVIVAL_URL") ?? "https://mnrrevival.derole.net").TrimEnd('/');
        static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(30) };
        static readonly ConcurrentDictionary<string, (int n, DateTime since)> Recent = new();   // imports per client address per hour
        static readonly ConcurrentDictionary<int, DateTime> Missing = new();   // ids Revival didn't have: not asked again for 10 minutes
        static readonly object Lock = new();   // one import at a time (the game asks for a creation's record, data and picture at once)
        public static readonly ConcurrentQueue<string> Log = new();   // recent imports, for the status page

        public static bool Mirrorable(int id) => id >= First && id < OwnFrom;

        /// <summary>Counts an import against the client's hourly allowance; false when it is used up.</summary>
        public static bool Allowed(string ip)
        {
            var r = Recent.AddOrUpdate(ip, (1, DateTime.UtcNow), (_, v) => DateTime.UtcNow - v.since > TimeSpan.FromHours(1) ? (1, DateTime.UtcNow) : (v.n + 1, v.since));
            return r.n <= 200;
        }

        /// <summary>Imports a creation (and its parent) from Revival. Returns an HTTP status and a short message.</summary>
        public static (int code, string msg) Import(Database database, IUGCStorage storage, int id, int depth = 0)
        {
            if (!Mirrorable(id)) return (400, "id");
            lock (Lock)
            {
                if (Missing.TryGetValue(id, out var when) && DateTime.UtcNow - when < TimeSpan.FromMinutes(10)) return (404, "not on Revival");
                XElement rec;
                byte[] data, preview = null;
                try
                {
                    rec = Record(id);
                    if (rec == null) { Missing[id] = DateTime.UtcNow; return (404, "not on Revival"); }
                    data = Http.GetByteArrayAsync($"{Revival}/player_creations/{id}/data.bin").Result;
                    try { preview = Http.GetByteArrayAsync($"{Revival}/player_creations/{id}/preview_image.png").Result; } catch (Exception) { }
                }
                catch (Exception) { return (502, "Revival unreachable"); }

                string A(string k) => (string)rec.Attribute(k) ?? "";
                int I(string k) => int.TryParse(A(k), out var v) ? v : 0;
                float F(string k) => float.TryParse(A(k), NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : 0;
                bool B(string k) => A(k) == "true";
                DateTime D(string k) => DateTime.TryParse(A(k), CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal, out var v) ? v : TimeUtils.Now;
                T E<T>(string k) where T : struct => Enum.TryParse<T>(A(k), true, out var v) ? v : default;

                if (A("platform") != "PS3" || !Enum.TryParse<PlayerCreationType>(A("player_creation_type"), out var type)
                    || (type != PlayerCreationType.KART && type != PlayerCreationType.CHARACTER && type != PlayerCreationType.TRACK))
                { Missing[id] = DateTime.UtcNow; return (400, "not an MNR PS3 character, kart or track"); }
                if (data.Length < 12 || data[0] != 'C' || data[1] != 'T' || data[2] != 'H') return (400, "data");

                var existing = database.PlayerCreations.FirstOrDefault(c => c.PlayerCreationId == id);
                if (existing != null) return Same(storage, id, data) ? (200, "exists") : (409, "another creation has this id here");

                var creator = Account(database, A("username"));
                if (creator == null) return (400, "no creator");
                var original = Account(database, A("original_player_username")) ?? creator;
                var parentPlayer = Account(database, A("parent_player_username")) ?? creator;
                int parent = Parent(database, storage, I("parent_creation_id"), depth);

                var creation = new PlayerCreationData
                {
                    PlayerCreationId = id, Name = A("name"), Description = A("description"), Type = type, Platform = Platform.PS3, IsMNR = true,
                    Tags = A("tags"), AutoTags = "", UserTags = "", DLCKeys = A("dlc_keys"), RequiresDLC = A("dlc_keys").Length > 0,
                    IsRemixable = B("is_remixable"), LongestHangTime = F("longest_hang_time"), LongestDrift = F("longest_drift"),
                    TrackTheme = I("track_theme"), AutoReset = B("auto_reset"), AI = B("ai"), NumLaps = I("num_laps"),
                    Speed = E<PlayerCreationSpeed>("speed"), RaceType = E<RaceType>("race_type"), WeaponSet = A("weapon_set"),
                    Difficulty = E<PlayerCreationDifficulty>("difficulty"), BattleKillCount = I("battle_kill_count"), BattleTimeLimit = I("battle_time_limit"),
                    BattleFriendlyFire = B("battle_friendly_fire"), NumRacers = I("num_racers"), MaxHumans = I("max_humans"),
                    AssociatedItemIds = "", AssociatedUsernames = "", AssociatedCoordinates = "", IsTeamPick = false,
                    LevelMode = I("level_mode"), ScoreboardMode = I("scoreboard_mode"), BestLapTime = F("best_lap_time"),
                    CreatedAt = D("created_at"), FirstPublished = D("first_published"), LastPublished = D("last_published"), UpdatedAt = D("updated_at"),
                    Version = Math.Max(1, I("version")), TrackId = id, PlayerId = creator.UserId, OriginalPlayerId = original.UserId,
                    ParentPlayerId = parentPlayer.UserId, ParentCreationId = parent, HasPreview = preview != null
                };
                database.PlayerCreations.Add(creation);
                database.SaveChanges();
                if (preview != null) storage.SavePlayerCreation(id, new MemoryStream(data), new MemoryStream(preview));
                else storage.SavePlayerCreation(id, new MemoryStream(data));
                var line = $"imported {type} {id} \"{creation.Name}\" by {creator.Username} from Revival" + (parent != 0 ? $" (remix of {parent})" : "");
                Serilog.Log.Information($"[mnrpc] {line}");
                Note(line);
                return (200, "ok");
            }
        }

        /// <summary>Fills in the parents of earlier imports that came without them. Returns how many were filled.</summary>
        public static int Repair(Database database, IUGCStorage storage)
        {
            int n = 0;
            foreach (var c in database.PlayerCreations.Where(c => c.PlayerCreationId >= First && c.PlayerCreationId < OwnFrom && c.ParentCreationId == 0).ToList())
            {
                XElement rec;
                try { rec = Record(c.PlayerCreationId); } catch (Exception) { continue; }
                if (rec == null || !int.TryParse((string)rec.Attribute("parent_creation_id"), out var p) || p == 0) continue;
                int parent; lock (Lock) parent = Parent(database, storage, p, 0);
                if (parent == 0) continue;
                c.ParentCreationId = parent;
                database.SaveChanges();
                var line = $"{c.PlayerCreationId} \"{c.Name}\": parent {parent} filled in";
                Serilog.Log.Information($"[mnrpc] {line}");
                Note(line); n++;
            }
            return n;
        }

        /// <summary>The parent's id once it is on this server (imported if need be, at most 3 generations up), else 0.</summary>
        static int Parent(Database database, IUGCStorage storage, int parent, int depth)
        {
            if (parent == 0) return 0;
            if (database.PlayerCreations.Any(c => c.PlayerCreationId == parent)) return parent;
            if (depth >= 3 || !Mirrorable(parent)) return 0;
            Import(database, storage, parent, depth + 1);   // (the lock is re-entrant)
            return database.PlayerCreations.Any(c => c.PlayerCreationId == parent) ? parent : 0;
        }

        static XElement Record(int id)
        {
            using var r = Http.GetAsync($"{Revival}/player_creations/{id}.xml").Result;
            if (!r.IsSuccessStatusCode) return null;
            var xml = XDocument.Parse(r.Content.ReadAsStringAsync().Result);
            if ((string)xml.Root?.Element("status")?.Element("id") != "0") return null;
            return xml.Root.Element("response")?.Element("player_creation");
        }

        static void Note(string line)
        {
            Log.Enqueue($"{DateTime.Now:yyyy-MM-dd HH:mm} {line}");
            while (Log.Count > 50) Log.TryDequeue(out _);
        }

        /// <summary>The account named after a creator, made the way a first sign-in makes one (without a PSN/RPCN id, so
        /// it becomes the player's when they sign in with that name).</summary>
        static User Account(Database database, string name)
        {
            name = (name ?? "").Trim();
            if (name.Length == 0 || name == "ufg") return null;
            var u = database.Users.FirstOrDefault(x => x.Username == name);
            if (u != null) return u;
            u = new User
            {
                UserId = database.Users.Count(x => x.Username != "ufg") + 11, Username = name, Quota = 30,
                CreatedAt = TimeUtils.Now, UpdatedAt = TimeUtils.Now, PolicyAccepted = false
            };
            database.Users.Add(u);
            database.SaveChanges();
            return u;
        }

        static bool Same(IUGCStorage storage, int id, byte[] d)
        {
            try
            {
                using var s = storage.LoadPlayerCreation(id, "data.bin");
                if (s == null) return false;
                using var ms = new MemoryStream(); s.CopyTo(ms);
                return SHA256.HashData(ms.ToArray()).AsSpan().SequenceEqual(SHA256.HashData(d));
            }
            catch (Exception) { return false; }
        }
    }

    public class MnrPcController(Database database, IUGCStorage storage) : Controller
    {
        [HttpGet]
        [Route("mnrpc/caps")]
        public IActionResult Caps() => Content("mnrpc import 1", "text/plain");

        [HttpPost]
        [Route("mnrpc/import")]
        public IActionResult Import(int id)
        {
            if (!MnrPcImport.Mirrorable(id)) return BadRequest("id");
            if (!MnrPcImport.Allowed(HttpContext.Connection.RemoteIpAddress?.ToString() ?? "?")) return StatusCode(429, "too many imports this hour");
            var (code, msg) = MnrPcImport.Import(database, storage, id);
            return StatusCode(code, msg);
        }

        /// <summary>Parents for imports made before they came along (from this PC only).</summary>
        [HttpPost]
        [Route("mnrpc/repair")]
        public IActionResult Repair()
        {
            if (!IPAddress.IsLoopback(HttpContext.Connection.RemoteIpAddress ?? IPAddress.None)) return NotFound();
            return Content($"{MnrPcImport.Repair(database, storage)} filled in", "text/plain");
        }

        /// <summary>The creation keyword tags and the chat filter, which stock PLGarage doesn't answer (the game then logs
        /// "failed to download profanity filter" and its tag lists stay empty). Taken from the game's own copies on the
        /// disc (DATA\CREATIONKEYWORDTAGS.XML, DATA\PROFANITYFILTER.XML), kept in GameResources by patch_plgarage.py.</summary>
        [HttpGet]
        [Route("tags.xml")]
        public IActionResult Tags()
        {
            var tags = Resource("mnrpc.tags.xml");
            return tags == null ? NotFound() : Result(tags);
        }

        [HttpGet]
        [Route("profanity_filters.xml")]
        public IActionResult ProfanityFilters()
        {
            var disc = Resource("mnrpc.profanity.xml");
            if (disc == null) return NotFound();
            return Result(new XElement("profanity_filters", disc.Elements("text_filter").Select(f =>
                new XElement("profanity_filter", new XAttribute("pattern", (string)f.Attribute("pattern") ?? ""), new XAttribute("replace", (string)f.Attribute("replace") ?? "")))));
        }

        static XElement Resource(string name)
        {
            var path = Path.Combine(AppContext.BaseDirectory, "GameResources", name);
            try { return System.IO.File.Exists(path) ? XDocument.Load(path).Root : null; } catch (Exception) { return null; }
        }

        ContentResult Result(XElement response)
        {
            var x = new XElement("result",
                new XElement("status", new XElement("id", 0), new XElement("message", "Successful completion")),
                new XElement("response", response));
            return Content("<?xml version=\"1.0\" encoding=\"UTF-8\"?>" + x.ToString(SaveOptions.DisableFormatting), "application/xml;charset=utf-8");
        }

        /// <summary>A page for the server's owner (this PC and the home network only): players, creations, recent imports.</summary>
        [HttpGet]
        [Route("mnrpc/status")]
        public IActionResult Status()
        {
            var ip = HttpContext.Connection.RemoteIpAddress ?? IPAddress.None;
            if (ip.IsIPv4MappedToIPv6) ip = ip.MapToIPv4();
            var b = ip.GetAddressBytes();
            bool local = IPAddress.IsLoopback(ip) || (b.Length == 4 && (b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] < 32) || (b[0] == 192 && b[1] == 168)));
            if (!local) return NotFound();

            string H(object o) => WebUtility.HtmlEncode(o?.ToString() ?? "");
            var cutoff = TimeUtils.Now.AddMinutes(-5);
            var online = database.Sessions.Where(s => s.LastPing > cutoff).Select(s => s.User.Username).Distinct().ToList();
            var players = database.Users.Where(u => u.Username != "ufg").ToList();
            var creations = database.PlayerCreations.Where(c => c.PlayerCreationId >= MnrPcImport.First).Select(c => new { c.PlayerCreationId, c.Type }).ToList();
            int mirrored = creations.Count(c => c.PlayerCreationId < MnrPcImport.OwnFrom), own = creations.Count - mirrored;
            var s = new StringBuilder();
            s.Append("<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'><title>MNR server</title>");
            s.Append("<style>body{font:15px system-ui,sans-serif;margin:16px;max-width:760px;background:#fff;color:#111}@media(prefers-color-scheme:dark){body{background:#151515;color:#eee}}h2{margin-top:24px}li{margin:2px 0}</style>");
            s.Append($"<h1>MNR server</h1><p>{H(DateTime.Now.ToString("yyyy-MM-dd HH:mm"))}</p>");
            s.Append($"<h2>Online now ({online.Count})</h2><ul>{string.Concat(online.Select(n => $"<li>{H(n)}</li>"))}</ul>");
            s.Append($"<h2>Creations</h2><p>{own} shared here, {mirrored} from MNR Revival: {creations.Count(c => c.Type == PlayerCreationType.TRACK)} tracks, " +
                     $"{creations.Count(c => c.Type == PlayerCreationType.KART)} karts, {creations.Count(c => c.Type == PlayerCreationType.CHARACTER)} characters.</p>");
            s.Append($"<h2>Recent imports</h2><ul>{string.Concat(MnrPcImport.Log.Reverse().Select(l => $"<li>{H(l)}</li>"))}</ul>");
            s.Append($"<h2>Accounts ({players.Count})</h2><ul>{string.Concat(players.OrderBy(u => u.UserId).Select(u => $"<li>{u.UserId} {H(u.Username)}</li>"))}</ul>");
            return Content(s.ToString(), "text/html; charset=utf-8");
        }
    }

    /// <summary>Imports a Revival creation the moment a player's game asks this server for it (a creation someone wears
    /// in a lobby, a track a room picks), so nobody has to install it through the launcher first. Runs before PLGarage's
    /// own handling of player_creations/&lt;id&gt;..., which then finds it.</summary>
    public class MnrPcStartup : IHostingStartup
    {
        public void Configure(IWebHostBuilder builder) => builder.ConfigureServices(s => s.AddTransient<IStartupFilter, Filter>());

        class Filter : IStartupFilter
        {
            public Action<IApplicationBuilder> Configure(Action<IApplicationBuilder> next) => app => { app.Use(OnTheFly); next(app); };
        }

        static readonly Regex CreationPath = new(@"^/player_creations/(\d+)(\.xml|/)", RegexOptions.Compiled);

        static async Task OnTheFly(HttpContext ctx, Func<Task> next)
        {
            var m = HttpMethods.IsGet(ctx.Request.Method) ? CreationPath.Match(ctx.Request.Path.Value ?? "") : Match.Empty;
            if (m.Success && int.TryParse(m.Groups[1].Value, out var id) && MnrPcImport.Mirrorable(id))
            {
                var db = ctx.RequestServices.GetService<Database>();
                var storage = ctx.RequestServices.GetService<IUGCStorage>();
                if (db != null && storage != null && !db.PlayerCreations.Any(c => c.PlayerCreationId == id)
                    && MnrPcImport.Allowed(ctx.Connection.RemoteIpAddress?.ToString() ?? "?"))
                    await Task.Run(() => MnrPcImport.Import(db, storage, id));
            }
            if ((ctx.Request.Path.Value ?? "").Contains("friends", StringComparison.OrdinalIgnoreCase))
            {   // the friends' creations and leaderboards: request and answer logged (a friend's Download list came out empty, 2026-11-01)
                var body = ctx.Response.Body; var copy = new MemoryStream(); ctx.Response.Body = copy;
                try { await next(); }
                finally
                {
                    copy.Position = 0; await copy.CopyToAsync(body); ctx.Response.Body = body;
                    var text = Encoding.UTF8.GetString(copy.ToArray());
                    Serilog.Log.Information($"[mnrpc] {ctx.Request.Method} {ctx.Request.Path}{ctx.Request.QueryString} -> {ctx.Response.StatusCode}, {copy.Length} bytes: {(text.Length > 400 ? text[..400] + "..." : text)}");
                }
                return;
            }
            await next();
        }
    }
}
