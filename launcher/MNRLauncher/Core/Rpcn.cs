using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace MNRLauncher.Core;

/// <summary>
/// A minimal client for RPCN, RPCS3's free stand-in for PSN (np.rpcs3.net), used to sign in with the player's own
/// RPCN account and get NP tickets: the signed proof of identity the game hands to MNR Revival's servers, which
/// accept RPCN tickets as RPCS3 players use them.
///
/// Protocol (as RPCS3's rpcn_client.cpp, protocol version 34): TLS 1.2 over TCP (port 31313); every packet is a
/// 15-byte little-endian header (u8 type, u16 command, u32 size incl. header, u64 id) and a payload. The server
/// first sends ServerInfo (u32 version). Login = npid\0 password\0 token\0; replies start with an error byte.
/// RequestTicket = service id\0, u32 cookie size, cookie; its reply is error byte, u32 size, ticket.
/// The password RPCN expects is RPCS3's derived one: PBKDF2-HMAC-SHA3-256 (200,000 rounds, fixed salt) in upper-case hex.
/// </summary>
public sealed class RpcnClient : IDisposable
{
    public const int ProtocolVersion = 34;   // RPCN 34 (Oct 2026: trophy sync, TSS) only added commands; login, tickets and signaling are unchanged
    public const string DefaultHost = "np.rpcs3.net:31313";

    /// <summary>RPCN packet types: request, reply, notification, or server info.</summary>
    enum PacketType : byte { Request, Reply, Notification, ServerInfo }
    /// <summary>RPCN protocol commands: login, terminate, create account, send token, friend operations, signaling, ticket, messaging, presence.</summary>
    enum Command : ushort { Login = 0, Terminate = 1, Create = 2, SendToken = 4, AddFriend = 8, RemoveFriend = 9, RequestSignalingInfos = 27, RequestTicket = 28, SendMessage = 29, SetPresence = 51 }
    /// <summary>Notification command ID for signaling helper (another player requesting our network address for P2P connection).</summary>
    public const ushort NotificationSignalingHelper = 12;   // another player asked RPCN for our address: punch through to it
    /// <summary>Friend notification command IDs: query, new, lost, status change, message received, presence change.</summary>
    public const ushort NotificationFriendQuery = 5, NotificationFriendNew = 6, NotificationFriendLost = 7, NotificationFriendStatus = 8,
                        NotificationMessageReceived = 10, NotificationFriendPresenceChanged = 11;

    /// <summary>
    /// A friend as RPCN reports them: online status, presence data (communication ID, title, status, comment), and custom data.
    /// </summary>
    public sealed class Friend
    {
        public bool Online;
        public string ComId = "", Title = "", Status = "", Comment = "";
        public byte[] Data = Array.Empty<byte>();
    }
    /// <summary>The friend list from the sign-in (kept up to date by <see cref="ApplyFriendNotification"/>); lock it to read.</summary>
    public Dictionary<string, Friend> Friends { get; } = new();
    public HashSet<string> RequestsSent { get; } = new();
    public HashSet<string> RequestsReceived { get; } = new();
    public HashSet<string> Blocked { get; } = new();

    /// <summary>
    /// RPCN error codes mapped to human-readable names, matching RPCS3's error table.
    /// </summary>
    static readonly string[] Errors =
    {
        "NoError", "Malformed", "Invalid", "InvalidInput", "TooSoon", "LoginError", "LoginAlreadyLoggedIn", "LoginInvalidUsername",
        "LoginInvalidPassword", "LoginInvalidToken", "CreationError", "CreationExistingUsername", "CreationBannedEmailProvider",
        "CreationExistingEmail", "RoomMissing", "RoomAlreadyJoined", "RoomFull", "RoomPasswordMismatch", "RoomPasswordMissing",
        "RoomGroupNoJoinLabel", "RoomGroupFull", "RoomGroupJoinLabelNotFound", "RoomGroupMaxSlotMismatch", "Unauthorized", "DbFail",
        "EmailFail", "NotFound", "Blocked", "AlreadyFriend", "ScoreNotBest", "ScoreInvalid", "ScoreHasData", "CondFail", "Unsupported",
    };
    /// <summary>Returns the human-readable name for an RPCN error code, or a generic "error N" string.</summary>
    public static string ErrorName(byte e) => e < Errors.Length ? Errors[e] : $"error {e}";

    /// <summary>TCP client connection to the RPCN server.</summary>
    TcpClient? tcp;
    /// <summary>SSL stream wrapping the TCP connection for encrypted communication.</summary>
    SslStream? ssl;
    /// <summary>Monotonically increasing packet ID for matching requests to replies.</summary>
    ulong nextId = 1;
    /// <summary>The RPCN server's negotiated protocol version.</summary>
    public int ServerVersion { get; private set; }
    /// <summary>The online name (NPID) of the signed-in user.</summary>
    public string OnlineName { get; private set; } = "";
    /// <summary>RPCN's numeric user ID, used in the game's UDP pings.</summary>
    public long UserId { get; private set; }               // RPCN's id for the account (the game's UDP pings carry it)
    /// <summary>The server's IP address as seen by this client.</summary>
    public IPAddress? ServerAddress => (tcp?.Client.RemoteEndPoint as IPEndPoint)?.Address;
    /// <summary>True until the reader thread detects that the connection has closed.</summary>
    public bool Alive { get; private set; } = true;
    /// <summary>Server notifications (command, payload) once <see cref="StartReader"/> runs.</summary>
    public event Action<ushort, byte[]>? Notification;
    /// <summary>Lock object for thread-safe packet writing.</summary>
    readonly object writeLock = new();
    /// <summary>Pending request ID -> TaskCompletionSource mapping for async request/reply matching.</summary>
    readonly Dictionary<ulong, TaskCompletionSource<byte[]>> pending = new();
    /// <summary>The background thread reading packets from the RPCN server.</summary>
    Thread? reader;

    /// <summary>
    /// Connects to the RPCN server at the given host, negotiates protocol version, and returns the client.
    /// Uses IPv4 only to avoid IPv6 hang on networks without IPv6 routing. Disables certificate validation (matching RPCS3).
    /// </summary>
    public static RpcnClient Connect(string host, int timeoutMs = 15000)
    {
        string h = host.Trim(); int port = 31313;
        int colon = h.LastIndexOf(':');
        if (colon > 0 && int.TryParse(h[(colon + 1)..], out int p)) { port = p; h = h[..colon]; }
        var c = new RpcnClient();
        c.tcp = new TcpClient(AddressFamily.InterNetwork) { ReceiveTimeout = timeoutMs, SendTimeout = timeoutMs };
        // IPv4 explicitly: np.rpcs3.net also has an IPv6 address, which hangs on networks without IPv6 routing
        var addrs = Dns.GetHostAddresses(h).Where(a => a.AddressFamily == AddressFamily.InterNetwork).ToArray();
        if (addrs.Length == 0) throw new IOException($"Couldn't find {h}.");
        try { if (!c.tcp.ConnectAsync(addrs, port).Wait(timeoutMs)) throw new IOException($"Couldn't reach {h}:{port}."); }
        catch (AggregateException e) { throw new IOException($"Couldn't reach {h}:{port}: {e.InnerException?.Message}"); }
        // RPCN's certificate isn't checked (RPCS3 doesn't either: SSL_VERIFY_NONE)
        c.ssl = new SslStream(c.tcp.GetStream(), false, (_, _, _, _) => true);
        c.ssl.AuthenticateAsClient(new SslClientAuthenticationOptions { TargetHost = h, EnabledSslProtocols = SslProtocols.Tls12 });
        var (type, _, _, data) = c.ReadPacket();
        if (type != PacketType.ServerInfo || data.Length != 4) throw new IOException("RPCN didn't introduce itself.");
        c.ServerVersion = BitConverter.ToInt32(data, 0);
        if (c.ServerVersion != ProtocolVersion)
            throw new IOException($"RPCN speaks protocol {c.ServerVersion}; the launcher speaks {ProtocolVersion}. A launcher update is needed.");
        return c;
    }

    /// <summary>
    /// Signs in to RPCN with the given NPID, derived password (PBKDF2-HMAC-SHA3-256 output), and session token.
    /// Parses the reply to extract online name, user ID, friends list, and friend request states.
    /// </summary>
    public void Login(string npid, string derivedPassword, string token)
    {
        var body = new List<byte>();
        foreach (var s in new[] { npid, derivedPassword, token ?? "" }) { body.AddRange(Encoding.UTF8.GetBytes(s)); body.Add(0); }
        var reply = Request(Command.Login, body.ToArray());
        if (reply.Length == 0) throw new IOException("RPCN sent an empty answer to the sign-in.");
        if (reply[0] != 0) throw new RpcnException(reply[0], $"RPCN refused the sign-in: {ErrorName(reply[0])}.");
        int pos = 1;
        OnlineName = ReadString(reply, ref pos);
        ReadString(reply, ref pos);   // avatar url
        if (pos + 8 <= reply.Length) UserId = BitConverter.ToInt64(reply, pos);
        pos += 8;
        try
        {   // then the friends: u32 count, each {name, u8 online, presence}; requests sent, received, blocked: u32 count + names
            lock (Friends)
            {
                uint n = BitConverter.ToUInt32(reply, pos); pos += 4;
                for (uint i = 0; i < n && pos < reply.Length; i++)
                {
                    var name = ReadString(reply, ref pos);
                    var f = new Friend { Online = reply[pos++] != 0 };
                    ReadPresence(reply, ref pos, f);
                    Friends[name] = f;
                }
                foreach (var set in new[] { RequestsSent, RequestsReceived, Blocked })
                {
                    uint m = BitConverter.ToUInt32(reply, pos); pos += 4;
                    for (uint i = 0; i < m && pos < reply.Length; i++) set.Add(ReadString(reply, ref pos));
                }
            }
        }
        catch (Exception) { }   // (an older server without the lists: no friends)
    }

    /// <summary>
    /// Parses a friend's presence data from an RPCN packet: communication ID, title, status, comment, and custom data.
    /// </summary>
    static void ReadPresence(byte[] b, ref int pos, Friend f)
    {
        f.ComId = b[pos] == 0 ? "" : Encoding.ASCII.GetString(b, pos, 12).TrimEnd('\0'); pos += 12;   // (no game: zeros + "_00")
        f.Title = ReadString(b, ref pos); f.Status = ReadString(b, ref pos); f.Comment = ReadString(b, ref pos);
        uint size = BitConverter.ToUInt32(b, pos); pos += 4;
        f.Data = b[pos..(pos + (int)Math.Min(size, 128))]; pos += (int)size;
    }

    /// <summary>
    /// Processes a friend notification from RPCN, updating the Friends dictionary and returning an event line for the game.
    /// Handles query, new, lost, status change, and presence change notifications.
    /// </summary>
    public string? ApplyFriendNotification(ushort cmd, byte[] d)
    {
        int pos = 0;
        lock (Friends)
        {
            switch (cmd)
            {
                case NotificationFriendQuery: { var n = ReadString(d, ref pos); RequestsReceived.Add(n); return $"query {n}"; }
                case NotificationFriendNew:
                {
                    bool on = d[pos++] != 0; var n = ReadString(d, ref pos);
                    RequestsReceived.Remove(n); RequestsSent.Remove(n); Friends[n] = new Friend { Online = on };
                    return $"new {n} {(on ? 1 : 0)}";
                }
                case NotificationFriendLost: { var n = ReadString(d, ref pos); RequestsReceived.Remove(n); RequestsSent.Remove(n); Friends.Remove(n); return $"lost {n}"; }
                case NotificationFriendStatus:
                {
                    bool on = d[pos++] != 0; pos += 8; var n = ReadString(d, ref pos);   // (u64 timestamp)
                    if (!Friends.ContainsKey(n)) return null;
                    Friends[n] = new Friend { Online = on };   // (presence cleared on on/offline, as RPCS3)
                    return $"status {n} {(on ? 1 : 0)}";
                }
                case NotificationFriendPresenceChanged:
                {
                    var n = ReadString(d, ref pos);
                    if (!Friends.TryGetValue(n, out var f)) return null;
                    ReadPresence(d, ref pos, f);
                    return $"presence {n} {PresenceLine(f)}";
                }
            }
        }
        return null;
    }

    /// <summary>Converts a byte array to a hex string, or "-" if empty.</summary>
    static string Hex(byte[] b) => b.Length == 0 ? "-" : Convert.ToHexString(b);
    /// <summary>Converts a string to a hex string (UTF-8 encoded).</summary>
    static string Hex(string s) => Hex(Encoding.UTF8.GetBytes(s));
    /// <summary>
    /// Formats a friend's presence as a single string for the game: "online comid title status comment data".
    /// Strings and data are hex-encoded; empty values are represented as "-".
    /// </summary>
    public static string PresenceLine(Friend f) => $"{(f.Online ? 1 : 0)} {(f.ComId.Length > 0 ? f.ComId : "-")} {Hex(f.Title)} {Hex(f.Status)} {Hex(f.Comment)} {Hex(f.Data)}";

    /// <summary>
    /// Parses a received message from RPCN and formats it as the game's event line: "msg sender comid mainType subType features data".
    /// Uses protobuf parsing to extract message details from the payload.
    /// </summary>
    public static string? MessageLine(byte[] d)
    {
        int pos = 0;
        var from = ReadString(d, ref pos);
        if (pos + 4 > d.Length) return null;
        uint size = BitConverter.ToUInt32(d, pos); pos += 4;
        var f = Protobuf.Fields(d, pos, (int)Math.Min(size, (uint)(d.Length - pos)));
        string com = f.TryGetValue(1, out var c) ? Encoding.UTF8.GetString(c) : "";
        uint Sub(int k) { if (!f.TryGetValue(k, out var m)) return 0; var g = Protobuf.Fields(m, 0, m.Length); return g.TryGetValue(1, out var v) ? (uint)Protobuf.Varint(v) : 0; }
        uint feat = f.TryGetValue(5, out var fe) ? (uint)Protobuf.Varint(fe) : 0;
        var data = f.TryGetValue(8, out var dd) ? dd : Array.Empty<byte>();
        return com.Length == 0 ? null : $"msg {from} {com} {Sub(3)} {Sub(4)} {feat} {Hex(data)}";
    }

    /// <summary>Sends or accepts a friend request to the given NPID. Returns RPCN's error code (0: success).</summary>
    public byte AddFriend(string npid) { var r = Request(Command.AddFriend, Encoding.UTF8.GetBytes(npid).Append((byte)0).ToArray()); return r.Length > 0 ? r[0] : (byte)1; }
    /// <summary>Removes a friend request from the given NPID. Returns RPCN's error code (0: success).</summary>
    public byte RemoveFriend(string npid) { var r = Request(Command.RemoveFriend, Encoding.UTF8.GetBytes(npid).Append((byte)0).ToArray()); return r.Length > 0 ? r[0] : (byte)1; }

    /// <summary>
    /// Sets our presence for friends: communication ID, title, status, comment, and custom data.
    /// The presence is serialized as protobuf and sent to the RPCN server.
    /// </summary>
    public void SetPresence(string comId, string title, string status, string comment, byte[] data)
    {
        var pb = new List<byte>();
        Protobuf.Bytes(pb, 1, Encoding.UTF8.GetBytes(title)); Protobuf.Bytes(pb, 2, Encoding.UTF8.GetBytes(status));
        Protobuf.Bytes(pb, 3, Encoding.UTF8.GetBytes(comment)); Protobuf.Bytes(pb, 4, data);
        var body = new List<byte>(Encoding.ASCII.GetBytes(comId.PadRight(12, '\0')[..12]));
        body.AddRange(BitConverter.GetBytes((uint)pb.Count)); body.AddRange(pb);
        Send(Command.SetPresence, body.ToArray());
    }

    /// <summary>
    /// Sends a message to one or more players via RPCN.
    /// The message includes communication ID, main/sub type, features, subject, body, and custom data.
    /// Recipients are specified as a list of NPID strings.
    /// </summary>
    public void SendMessage(string comId, uint mainType, uint subType, uint features, string subject, string bodyText, byte[] data, IEnumerable<string> npids)
    {
        var m = new List<byte>();
        Protobuf.Bytes(m, 1, Encoding.UTF8.GetBytes(comId));
        var mt = new List<byte>(); Protobuf.Varint(mt, 1, mainType); Protobuf.Message(m, 3, mt.ToArray());
        var st = new List<byte>(); Protobuf.Varint(st, 1, subType); Protobuf.Message(m, 4, st.ToArray());
        Protobuf.Varint(m, 5, features);
        Protobuf.Bytes(m, 6, Encoding.UTF8.GetBytes(subject)); Protobuf.Bytes(m, 7, Encoding.UTF8.GetBytes(bodyText)); Protobuf.Bytes(m, 8, data);
        var req = new List<byte>(); Protobuf.Bytes(req, 1, m.ToArray());
        foreach (var n in npids) Protobuf.Bytes(req, 2, Encoding.UTF8.GetBytes(n));
        var body = new List<byte>(BitConverter.GetBytes((uint)req.Count)); body.AddRange(req);
        Send(Command.SendMessage, body.ToArray());
    }

    /// <summary>
    /// Creates a new RPCN account with the given NPID, derived password, and email.
    /// The online name is set to the NPID, and a default avatar URL is used.
    /// RPCN emails a verification token to the address, required for the first sign-in.
    /// </summary>
    public void CreateAccount(string npid, string derivedPassword, string email)
    {
        var body = new List<byte>();
        foreach (var s in new[] { npid, derivedPassword, npid, "https://rpcs3.net/cdn/netplay/DefaultAvatar.png", email }) { body.AddRange(Encoding.UTF8.GetBytes(s)); body.Add(0); }
        var reply = Request(Command.Create, body.ToArray());
        if (reply.Length == 0) throw new IOException("RPCN sent an empty answer to the account creation.");
        if (reply[0] != 0) throw new RpcnException(reply[0], reply[0] switch
        {
            11 => "An RPCN account with that user name already exists.",
            12 => "RPCN doesn't accept that e-mail provider; use another address.",
            13 => "An RPCN account with that e-mail address already exists.",
            _ => $"RPCN couldn't create the account: {ErrorName(reply[0])}.",
        });
    }

    /// <summary>
    /// Requests RPCN to resend the account verification token via email.
    /// Uses the NPID and derived password for authentication.
    /// </summary>
    public void ResendToken(string npid, string derivedPassword)
    {
        var body = new List<byte>();
        foreach (var s in new[] { npid, derivedPassword }) { body.AddRange(Encoding.UTF8.GetBytes(s)); body.Add(0); }
        var reply = Request(Command.SendToken, body.ToArray());
        if (reply.Length == 0) throw new IOException("RPCN sent an empty answer.");
        if (reply[0] != 0) throw new RpcnException(reply[0], $"RPCN didn't resend the token: {ErrorName(reply[0])}.");
    }

    /// <summary>
    /// Validates an RPCN username per RPCS3 rules: 3-16 characters, ASCII letters/digits/hyphen/underscore.
    /// </summary>
    public static bool ValidUserName(string n) => n.Length is >= 3 and <= 16 && n.All(ch => ch < 128 && (char.IsLetterOrDigit(ch) || ch == '-' || ch == '_'));
    /// <summary>Validates an email address using a standard regex pattern.</summary>
    public static bool ValidEmail(string e) => System.Text.RegularExpressions.Regex.IsMatch(e, @"^[a-zA-Z0-9.!#$%&'*+/=?^_`{|}~-]+@[a-zA-Z0-9-]+(?:\.[a-zA-Z0-9-]+)*$");

    /// <summary>
    /// Starts the background reader thread that processes incoming RPCN packets.
    /// Replies are routed to their pending requests; notifications are dispatched to the Notification event.
    /// Used by the session kept open while the game runs.
    /// </summary>
    public void StartReader()
    {
        tcp!.ReceiveTimeout = 0;
        reader = new Thread(() =>
        {
            try
            {
                for (;;)
                {
                    var (type, command, pid, data) = ReadPacket();
                    if (type == PacketType.Reply)
                    {
                        TaskCompletionSource<byte[]>? t;
                        lock (pending) { pending.Remove(pid, out t); }
                        t?.TrySetResult(data);
                    }
                    else if (type == PacketType.Notification) { try { Notification?.Invoke(command, data); } catch { } }
                }
            }
            catch (Exception)
            {
                Alive = false;
                lock (pending) { foreach (var t in pending.Values) t.TrySetException(new IOException("RPCN closed the connection.")); pending.Clear(); }
            }
        }) { IsBackground = true, Name = "RPCN reader" };
        reader.Start();
    }

    /// <summary>
    /// Requests the RPCN server for another player's network address (IP and port) for P2P connection.
    /// Returns null if the player is offline or the request fails.
    /// </summary>
    public (IPAddress addr, int port)? RequestSignalingInfos(string npid)
    {
        var body = new List<byte>(Encoding.UTF8.GetBytes(npid)) { 0 };
        var reply = Request(Command.RequestSignalingInfos, body.ToArray());
        if (reply.Length < 5 || reply[0] != 0) return null;
        return ParseSignalingAddr(Protobuf.Fields(reply, 5, (int)BitConverter.ToUInt32(reply, 1)));
    }

    /// <summary>
    /// Parses a SignalingHelper notification payload from RPCN to extract the requesting player's NPID and network address.
    /// The payload contains a protobuf-encoded MatchingSignalingInfo structure.
    /// </summary>
    public static (string npid, IPAddress addr, int port)? ParseSignalingHelper(byte[] data)
    {
        if (data.Length < 4) return null;
        var f = Protobuf.Fields(data, 4, (int)Math.Min(BitConverter.ToUInt32(data, 0), (uint)(data.Length - 4)));
        if (!f.TryGetValue(1, out var npid) || !f.TryGetValue(2, out var addr)) return null;
        var a = ParseSignalingAddr(Protobuf.Fields(addr, 0, addr.Length));
        return a == null ? null : (Encoding.UTF8.GetString(npid), a.Value.addr, a.Value.port);
    }

    /// <summary>
    /// Parses a SignalingAddr protobuf structure to extract an IP address and port number.
    /// The IP can be IPv4 (4 bytes) or IPv6 (16 bytes).
    /// </summary>
    static (IPAddress addr, int port)? ParseSignalingAddr(Dictionary<int, byte[]> f)
    {
        if (!f.TryGetValue(1, out var ip) || (ip.Length != 4 && ip.Length != 16)) return null;
        int port = 0;
        if (f.TryGetValue(2, out var pm)) { var pf = Protobuf.Fields(pm, 0, pm.Length); if (pf.TryGetValue(1, out var v)) port = (int)Protobuf.Varint(v); }
        return (new IPAddress(ip), port);
    }

    /// <summary>
    /// Requests an NP ticket from RPCN for the given service ID and authentication cookie.
    /// The ticket is a signed proof of identity that the game uses for server authentication.
    /// </summary>
    public byte[] RequestTicket(string serviceId, byte[] cookie)
    {
        var body = new List<byte>();
        body.AddRange(Encoding.UTF8.GetBytes(serviceId)); body.Add(0);
        body.AddRange(BitConverter.GetBytes((uint)cookie.Length)); body.AddRange(cookie);
        var reply = Request(Command.RequestTicket, body.ToArray());
        if (reply.Length == 0) throw new IOException("RPCN sent an empty ticket.");
        if (reply[0] != 0) throw new RpcnException(reply[0], $"RPCN refused the ticket: {ErrorName(reply[0])}.");
        if (reply.Length >= 5)
        {
            uint n = BitConverter.ToUInt32(reply, 1);
            if (n == reply.Length - 5) return reply[5..];
        }
        return reply[1..];
    }

    /// <summary>
    /// Sends a fire-and-forget request to RPCN (no reply waited for). Used for presence updates and messages.
    /// The reader thread will drop the reply if one arrives.
    /// </summary>
    void Send(Command cmd, byte[] payload)
    {
        ulong id;
        lock (writeLock) id = nextId++;
        var pkt = new byte[15 + payload.Length];
        pkt[0] = (byte)PacketType.Request;
        BitConverter.GetBytes((ushort)cmd).CopyTo(pkt, 1);
        BitConverter.GetBytes((uint)pkt.Length).CopyTo(pkt, 3);
        BitConverter.GetBytes(id).CopyTo(pkt, 7);
        payload.CopyTo(pkt, 15);
        lock (writeLock) { ssl!.Write(pkt); ssl.Flush(); }
    }

    /// <summary>
    /// Sends a request to RPCN and waits for the reply (up to 20 seconds).
    /// If the reader thread is running, the reply is matched via TaskCompletionSource; otherwise, packets are read synchronously.
    /// </summary>
    byte[] Request(Command cmd, byte[] payload)
    {
        ulong id;
        lock (writeLock) id = nextId++;
        var pkt = new byte[15 + payload.Length];
        pkt[0] = (byte)PacketType.Request;
        BitConverter.GetBytes((ushort)cmd).CopyTo(pkt, 1);
        BitConverter.GetBytes((uint)pkt.Length).CopyTo(pkt, 3);
        BitConverter.GetBytes(id).CopyTo(pkt, 7);
        payload.CopyTo(pkt, 15);
        if (reader != null)
        {
            var t = new TaskCompletionSource<byte[]>(TaskCreationOptions.RunContinuationsAsynchronously);
            lock (pending) pending[id] = t;
            lock (writeLock) { ssl!.Write(pkt); ssl.Flush(); }
            if (!t.Task.Wait(20000)) { lock (pending) pending.Remove(id); throw new IOException("RPCN didn't answer."); }
            return t.Task.Result;
        }
        lock (writeLock) { ssl!.Write(pkt); ssl.Flush(); }
        for (;;)   // notifications (friends, messages) may arrive first
        {
            var (type, command, pid, data) = ReadPacket();
            if (type == PacketType.Reply && pid == id) return data;
        }
    }

    /// <summary>Reads a single RPCN packet from the SSL stream: 15-byte header + payload.</summary>
    (PacketType, ushort, ulong, byte[]) ReadPacket()
    {
        var hdr = ReadExact(15);
        uint size = BitConverter.ToUInt32(hdr, 3);
        if (size < 15 || size > 16 << 20) throw new IOException("RPCN sent a malformed packet.");
        return ((PacketType)hdr[0], BitConverter.ToUInt16(hdr, 1), BitConverter.ToUInt64(hdr, 7), ReadExact((int)size - 15));
    }

    /// <summary>Reads exactly <paramref name="n"/> bytes from the SSL stream, blocking until all bytes are received.</summary>
    byte[] ReadExact(int n)
    {
        var b = new byte[n]; int got = 0;
        while (got < n)
        {
            int r = ssl!.Read(b, got, n - got);
            if (r <= 0) throw new IOException("RPCN closed the connection.");
            got += r;
        }
        return b;
    }

    /// <summary>Reads a null-terminated UTF-8 string from the buffer, advancing pos past the null terminator.</summary>
    static string ReadString(byte[] b, ref int pos)
    {
        int end = Array.IndexOf(b, (byte)0, pos);
        if (end < 0) end = b.Length;
        var s = Encoding.UTF8.GetString(b, pos, end - pos);
        pos = Math.Min(b.Length, end + 1);
        return s;
    }

    /// <summary>Closes the SSL stream and the TCP connection.</summary>
    public void Dispose()
    {
        try { ssl?.Dispose(); } catch { }
        try { tcp?.Dispose(); } catch { }
    }

    /// <summary>
    /// Derives the RPCN password per RPCS3's algorithm: PBKDF2-HMAC-SHA3-256 with 200,000 rounds.
    /// Uses a fixed salt "No matter where you go, everybody's connected." and returns 32 bytes as upper-case hex.
    /// </summary>
    public static string DerivePassword(string password)
    {
        var key = Pbkdf2Sha3(Encoding.UTF8.GetBytes(password), Encoding.UTF8.GetBytes("No matter where you go, everybody's connected."), 200_000);
        return Convert.ToHexString(key);
    }

    /// <summary>
    /// Computes PBKDF2-HMAC-SHA3-256: iterates HMAC(SHA3-256, password, salt) for the given number of rounds,
    /// XORing each block together to produce a 32-byte derived key.
    /// </summary>
    static byte[] Pbkdf2Sha3(byte[] password, byte[] salt, int rounds)
    {   // one 32-byte block: T = U1 ^ U2 ^ ... with U1 = HMAC(P, S || 00000001), Un = HMAC(P, Un-1)
        const int Rate = 136;
        var key = password.Length > Rate ? Sha3.Hash256(password) : password;
        var ipad = new byte[Rate]; var opad = new byte[Rate];
        for (int i = 0; i < Rate; i++) { byte k = i < key.Length ? key[i] : (byte)0; ipad[i] = (byte)(k ^ 0x36); opad[i] = (byte)(k ^ 0x5c); }
        var inner = new Sha3(); inner.Absorb(ipad);   // states after the key blocks, reused every round
        var outer = new Sha3(); outer.Absorb(opad);
        byte[] Hmac(byte[] msg) { var a = inner.Clone(); a.Absorb(msg); var ih = a.Final(); var b = outer.Clone(); b.Absorb(ih); return b.Final(); }
        var u = Hmac(salt.Concat(new byte[] { 0, 0, 0, 1 }).ToArray());
        var t = (byte[])u.Clone();
        for (int r = 1; r < rounds; r++) { u = Hmac(u); for (int i = 0; i < t.Length; i++) t[i] ^= u[i]; }
        return t;
    }
}

/// <summary>
/// Minimal protobuf parser/writer for RPCN's small messages.
/// Supports varint decoding, field parsing (length-delimited and fixed-size), and basic message construction.
/// </summary>
static class Protobuf
{
    /// <summary>Parses protobuf fields from a byte range, returning a dictionary of field number -> raw bytes.</summary>
    public static Dictionary<int, byte[]> Fields(byte[] b, int pos, int len)
    {
        var f = new Dictionary<int, byte[]>();
        int end = Math.Min(b.Length, pos + len);
        while (pos < end)
        {
            ulong key = Varint(b, ref pos); int field = (int)(key >> 3), wire = (int)(key & 7);
            if (wire == 0) { int s = pos; Varint(b, ref pos); f[field] = b[s..pos]; }
            else if (wire == 2) { int l = (int)Varint(b, ref pos); if (pos + l > end) break; f[field] = b[pos..(pos + l)]; pos += l; }
            else if (wire == 5) pos += 4;
            else if (wire == 1) pos += 8;
            else break;
        }
        return f;
    }
    /// <summary>Reads a protobuf varint from the beginning of the buffer.</summary>
    public static ulong Varint(byte[] b) { int p = 0; return Varint(b, ref p); }
    /// <summary>Reads a protobuf varint from the buffer, advancing pos. Returns the decoded value.</summary>
    static ulong Varint(byte[] b, ref int pos)
    {
        ulong v = 0; int shift = 0;
        while (pos < b.Length) { byte x = b[pos++]; v |= (ulong)(x & 0x7F) << shift; if ((x & 0x80) == 0) break; shift += 7; }
        return v;
    }
    // writing (proto3: empty fields are left out)
    /// <summary>Writes a raw protobuf varint to the output list.</summary>
    static void Raw(List<byte> o, ulong v) { while (v >= 0x80) { o.Add((byte)(v | 0x80)); v >>= 7; } o.Add((byte)v); }
    /// <summary>Writes a protobuf varint field (field number + value) to the output list.</summary>
    public static void Varint(List<byte> o, int field, ulong v) { if (v == 0) return; Raw(o, (ulong)field << 3); Raw(o, v); }
    /// <summary>Writes a protobuf bytes field (field number + length-delimited value) to the output list.</summary>
    public static void Bytes(List<byte> o, int field, byte[] v) { if (v.Length == 0) return; Message(o, field, v); }
    /// <summary>Writes a protobuf length-delimited message field to the output list.</summary>
    public static void Message(List<byte> o, int field, byte[] v) { Raw(o, (ulong)field << 3 | 2); Raw(o, (ulong)v.Length); o.AddRange(v); }   // (written even when empty)
}

/// <summary>
/// Exception thrown by RPCN operations containing the server's error code.
/// </summary>
public class RpcnException : IOException
{
    /// <summary>The RPCN error code returned by the server.</summary>
    public byte Code { get; }
    /// <summary>Creates a new RpcnException with the given error code and message.</summary>
    public RpcnException(byte code, string message) : base(message) { Code = code; }
}

/// <summary>
/// SHA3-256 implementation using Keccak-f[1600] permutation with rate 136 bytes.
/// Required because Windows 10 has no built-in SHA-3 support. Used for PBKDF2 password derivation.
/// </summary>
sealed class Sha3
{
    readonly ulong[] st = new ulong[25];
    readonly byte[] buf = new byte[136];
    int fill;

    /// <summary>Creates a deep copy of this SHA3 instance for parallel HMAC computation.</summary>
    public Sha3 Clone() { var c = new Sha3(); Array.Copy(st, c.st, 25); Array.Copy(buf, c.buf, fill); c.fill = fill; return c; }

    /// <summary>Absorbs data into the SHA3 state, processing full blocks as they arrive.</summary>
    public void Absorb(byte[] data)
    {
        foreach (var x in data)
        {
            buf[fill++] = x;
            if (fill == 136) { Block(); fill = 0; }
        }
    }

    /// <summary>Finishes the hash and returns the 32-byte digest.</summary>
    public byte[] Final()
    {
        Array.Clear(buf, fill, 136 - fill);
        buf[fill] ^= 0x06; buf[135] ^= 0x80;
        Block();
        var o = new byte[32];
        for (int i = 0; i < 4; i++) BitConverter.GetBytes(st[i]).CopyTo(o, i * 8);
        return o;
    }

    public static byte[] Hash256(byte[] data) { var h = new Sha3(); h.Absorb(data); return h.Final(); }

    void Block()
    {
        for (int i = 0; i < 17; i++) st[i] ^= BitConverter.ToUInt64(buf, i * 8);
        Keccak(st);
    }

    static readonly ulong[] RC =
    {
        0x0000000000000001, 0x0000000000008082, 0x800000000000808A, 0x8000000080008000, 0x000000000000808B, 0x0000000080000001,
        0x8000000080008081, 0x8000000000008009, 0x000000000000008A, 0x0000000000000088, 0x0000000080008009, 0x000000008000000A,
        0x000000008000808B, 0x800000000000008B, 0x8000000000008089, 0x8000000000008003, 0x8000000000008002, 0x8000000000000080,
        0x000000000000800A, 0x800000008000000A, 0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008,
    };
    static readonly int[] Rot = { 0, 1, 62, 28, 27, 36, 44, 6, 55, 20, 3, 10, 43, 25, 39, 41, 45, 15, 21, 8, 18, 2, 61, 56, 14 };

    static void Keccak(ulong[] a)
    {
        var c = new ulong[5]; var b = new ulong[25];
        for (int round = 0; round < 24; round++)
        {
            for (int x = 0; x < 5; x++) c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
            for (int x = 0; x < 5; x++)
            {
                ulong d = c[(x + 4) % 5] ^ ((c[(x + 1) % 5] << 1) | (c[(x + 1) % 5] >> 63));
                for (int y = 0; y < 25; y += 5) a[y + x] ^= d;
            }
            for (int x = 0; x < 5; x++)
                for (int y = 0; y < 5; y++)
                {
                    int r = Rot[x + 5 * y]; ulong v = a[x + 5 * y];
                    b[y + 5 * ((2 * x + 3 * y) % 5)] = r == 0 ? v : (v << r) | (v >> (64 - r));
                }
            for (int y = 0; y < 25; y += 5)
                for (int x = 0; x < 5; x++) a[y + x] = b[y + x] ^ (~b[y + (x + 1) % 5] & b[y + (x + 2) % 5]);
            a[0] ^= RC[round];
        }
    }
}

/// <summary>
/// Hands NP tickets to the running game: mnr.exe (MNR_NP_HELPER=127.0.0.1:port) asks
/// "GET /ticket?service=...&amp;cookie=hex" whenever it signs in, and gets a fresh ticket from RPCN with the saved account.
/// </summary>
public sealed class TicketService : IDisposable
{
    readonly TcpListener listener = new(IPAddress.Loopback, 0);
    readonly Func<(string host, string npid, string pass, string token)> account;
    readonly CancellationTokenSource stop = new();
    public int Port { get; }
    public string LastStatus { get; private set; } = "";
    public event Action<string>? Status;

    /// <summary>Starts the local ticket service on a free port.</summary>
    public TicketService(Func<(string, string, string, string)> account)
    {
        this.account = account;
        listener.Start();
        Port = ((IPEndPoint)listener.LocalEndpoint).Port;
        Task.Run(Loop);
    }

    void Report(string s)
    {
        LastStatus = s; Status?.Invoke(s);
        try { File.AppendAllText(Path.Combine(Paths.LogsDir, "rpcn.log"), $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {s}{Environment.NewLine}"); } catch (Exception) { }   // why a sign-in failed, after the fact
    }

    async Task Loop()
    {
        while (!stop.IsCancellationRequested)
        {
            TcpClient c;
            try { c = await listener.AcceptTcpClientAsync(stop.Token); } catch { return; }
            _ = Task.Run(() => Serve(c));
        }
    }

    void Serve(TcpClient c)
    {
        using (c)
        {
            var s = c.GetStream();
            s.ReadTimeout = 10000;
            var req = new StringBuilder(); var one = new byte[1];
            while (!req.ToString().EndsWith("\r\n\r\n") && req.Length < 8192 && s.Read(one, 0, 1) == 1) req.Append((char)one[0]);
            var line = req.ToString().Split("\r\n")[0].Split(' ');
            byte[] body; string status;
            try
            {
                if (line.Length >= 2 && line[1] == "/rpcn/info")
                {   // for the game's P2P port (p2p.c): our RPCN user id and the server's IPv4 address
                    var ses = Session();
                    var ip = ses.ServerAddress?.MapToIPv4().ToString() ?? "";
                    WriteReply(s, "200 OK", Encoding.ASCII.GetBytes($"{ses.UserId} {ip}"));
                    return;
                }
                if (line.Length >= 2 && line[1] == "/rpcn/events")
                {   // queued SignalingHelper notifications: "helper <npid> <ipv4> <port>" per line
                    try { Session(); } catch (Exception) { }
                    var sb = new StringBuilder();
                    while (events.TryDequeue(out var ev)) sb.Append(ev).Append('\n');
                    WriteReply(s, "200 OK", Encoding.ASCII.GetBytes(sb.ToString()));
                    return;
                }
                if (line.Length >= 2 && line[1].StartsWith("/rpcn/friend"))
                {   // friends for the game's sceNpBasic calls (host.c): see Friends()
                    WriteReply(s, "200 OK", Encoding.UTF8.GetBytes(Friends(line[1])));
                    return;
                }
                if (line.Length < 2 || !line[1].StartsWith("/ticket?")) throw new IOException("unknown request");
                var q = line[1][8..].Split('&').Select(kv => kv.Split('=', 2)).Where(kv => kv.Length == 2).ToDictionary(kv => kv[0], kv => Uri.UnescapeDataString(kv[1]));
                string service = q.GetValueOrDefault("service", "");
                byte[] cookie = q.TryGetValue("cookie", out var hex) && hex.Length % 2 == 0 ? Convert.FromHexString(hex) : Array.Empty<byte>();
                body = Fetch(service, cookie);
                status = "200 OK";
                Report($"Signed in to RPCN; ticket sent to the game ({DateTime.Now:t}).");
            }
            catch (Exception e)
            {
                body = Encoding.UTF8.GetBytes(e.Message); status = "503 Unavailable";
                Report("RPCN sign-in failed: " + e.Message);
            }
            WriteReply(s, status, body);
        }
    }

    static void WriteReply(Stream s, string status, byte[] body)
    {
        var head = Encoding.ASCII.GetBytes($"HTTP/1.0 {status}\r\nContent-Type: application/octet-stream\r\nContent-Length: {body.Length}\r\nConnection: close\r\n\r\n");
        try { s.Write(head); s.Write(body); } catch { }
    }

    // One RPCN session while the game runs (RPCN allows one sign-in per account): tickets, the user id for the game's
    // UDP pings, and SignalingHelper notifications all go through it. Closed when the game exits (EndSession).
    readonly object sessionLock = new();
    RpcnClient? session;
    DateTime lastFailure;
    readonly System.Collections.Concurrent.ConcurrentQueue<string> events = new();

    RpcnClient Session()
    {
        lock (sessionLock)
        {
            if (session is { Alive: true }) return session;
            session?.Dispose(); session = null;
            if (DateTime.Now - lastFailure < TimeSpan.FromSeconds(5)) throw new IOException("RPCN sign-in failed a moment ago.");
            var (host, npid, pass, token) = account();
            if (string.IsNullOrEmpty(npid) || string.IsNullOrEmpty(pass)) throw new IOException("No RPCN account is set up in the launcher (Settings > Online).");
            try
            {
                var c = RpcnClient.Connect(string.IsNullOrWhiteSpace(host) ? RpcnClient.DefaultHost : host);
                c.Login(npid, pass, token);
                c.Notification += (cmd, data) =>
                {
                    if (cmd == RpcnClient.NotificationSignalingHelper)
                    {
                        var h = RpcnClient.ParseSignalingHelper(data);
                        if (h != null) events.Enqueue($"helper {h.Value.npid} {h.Value.addr.MapToIPv4()} {h.Value.port}");
                        return;
                    }
                    var line = cmd == RpcnClient.NotificationMessageReceived ? RpcnClient.MessageLine(data) : c.ApplyFriendNotification(cmd, data);
                    if (line != null) friendEvents.Enqueue(line);
                };
                c.StartReader();
                session = c;
                return c;
            }
            catch (Exception) { lastFailure = DateTime.Now; throw; }
        }
    }

    /// <summary>The game has exited: sign out of RPCN.</summary>
    public void EndSession()
    {
        lock (sessionLock) { session?.Dispose(); session = null; }
        while (events.TryDequeue(out _)) { }
        while (friendEvents.TryDequeue(out _)) { }
    }

    readonly System.Collections.Concurrent.ConcurrentQueue<string> friendEvents = new();

    /// <summary>The game's friend requests (as RPCS3 answers sceNpBasic from its RPCN client), one line per item:
    ///   /rpcn/friends                 "friend npid online comid title status comment data" (hex, "-" when empty),
    ///                                 "reqin npid", "reqout npid", "blocked npid"
    ///   /rpcn/friendevents            queued "query|new|lost|status|presence|msg ..." lines (RpcnClient.ApplyFriendNotification, MessageLine)
    ///   /rpcn/friendpresence?com=&amp;title=&amp;status=&amp;data=   our presence (hex values)
    ///   /rpcn/friendadd?npid=          a friend request; answers "ok" or RPCN's error name
    ///   /rpcn/friendmsg?com=&amp;main=&amp;sub=&amp;feat=&amp;to=a,b&amp;subject=&amp;body=&amp;data=   a message (invite) to players</summary>
    string Friends(string url)
    {
        var path = url.Split('?')[0];
        var q = url.Contains('?') ? url[(url.IndexOf('?') + 1)..].Split('&').Select(kv => kv.Split('=', 2)).Where(kv => kv.Length == 2)
                    .ToDictionary(kv => kv[0], kv => Uri.UnescapeDataString(kv[1])) : new Dictionary<string, string>();
        byte[] H(string k) => q.TryGetValue(k, out var v) && v != "-" && v.Length % 2 == 0 ? Convert.FromHexString(v) : Array.Empty<byte>();
        string T(string k) => Encoding.UTF8.GetString(H(k));
        uint U(string k) => q.TryGetValue(k, out var v) && uint.TryParse(v, out var n) ? n : 0;
        var ses = Session();
        switch (path)
        {
            case "/rpcn/friends":
            {
                var sb = new StringBuilder();
                lock (ses.Friends)
                {
                    foreach (var (n, f) in ses.Friends) sb.Append($"friend {n} {RpcnClient.PresenceLine(f)}\n");
                    foreach (var n in ses.RequestsReceived) sb.Append($"reqin {n}\n");
                    foreach (var n in ses.RequestsSent) sb.Append($"reqout {n}\n");
                    foreach (var n in ses.Blocked) sb.Append($"blocked {n}\n");
                }
                return sb.ToString();
            }
            case "/rpcn/friendevents":
            {
                var sb = new StringBuilder();
                while (friendEvents.TryDequeue(out var ev)) sb.Append(ev).Append('\n');
                return sb.ToString();
            }
            case "/rpcn/friendpresence":
                ses.SetPresence(q.GetValueOrDefault("com", ""), T("title"), T("status"), "", H("data"));
                return "ok";
            case "/rpcn/friendadd":
            {
                var npid = q.GetValueOrDefault("npid", "");
                byte e = ses.AddFriend(npid);
                if (e == 0) lock (ses.Friends) if (!ses.Friends.ContainsKey(npid)) ses.RequestsSent.Add(npid);
                Report(e == 0 ? $"Friend request to {npid} sent (or accepted)." : $"Friend request to {npid}: {RpcnClient.ErrorName(e)}.");
                return e == 0 ? "ok" : RpcnClient.ErrorName(e);
            }
            case "/rpcn/friendmsg":
                ses.SendMessage(q.GetValueOrDefault("com", ""), U("main"), U("sub"), U("feat"), T("subject"), T("body"), H("data"),
                                q.GetValueOrDefault("to", "").Split(',', StringSplitOptions.RemoveEmptyEntries));
                return "ok";
        }
        throw new IOException("unknown request");
    }

    /// <summary>Requests a ticket for the service, retrying once on a fresh session if the kept one dropped.</summary>
    public byte[] Fetch(string service, byte[] cookie)
    {
        try { return Session().RequestTicket(service, cookie); }
        catch (IOException) when (session is not { Alive: true })
        {   // the kept session had dropped: once more on a fresh one
            lock (sessionLock) { session?.Dispose(); session = null; lastFailure = default; }
            return Session().RequestTicket(service, cookie);
        }
    }

    /// <summary>Stops the listener and the service loop.</summary>
    public void Dispose() { stop.Cancel(); try { listener.Stop(); } catch { } }
}
