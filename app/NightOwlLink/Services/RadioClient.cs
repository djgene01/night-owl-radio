using System.Net;
using System.Net.Http;
using System.Net.Http.Json;
using System.Net.Sockets;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace NightOwlLink.Services;

/// <summary>What the radio reports on /api/status (only the fields this app uses).</summary>
public sealed class RadioStatus
{
    [JsonPropertyName("station")] public string Station { get; set; } = "";
    [JsonPropertyName("title")] public string Title { get; set; } = "";
    [JsonPropertyName("time")] public string Time { get; set; } = "";
    [JsonPropertyName("vol")] public int Volume { get; set; }
    [JsonPropertyName("src")] public string Source { get; set; } = "";
    [JsonPropertyName("lap")] public bool LaptopAvailable { get; set; }
    [JsonPropertyName("ibuf")] public int InputBuffer { get; set; }
    [JsonPropertyName("active")] public bool Active { get; set; }
    [JsonPropertyName("paused")] public bool Paused { get; set; }
    [JsonPropertyName("kbps")] public int Kbps { get; set; }
    [JsonPropertyName("ip")] public string Ip { get; set; } = "";
    [JsonPropertyName("bass")] public int Bass { get; set; }
    [JsonPropertyName("mid")] public int Mid { get; set; }
    [JsonPropertyName("treble")] public int Treble { get; set; }

    public bool IsLaptop => Source == "laptop";
}

/// <summary>Talks to the radio's web API. The radio answers every call with its status.</summary>
public sealed class RadioClient
{
    private readonly HttpClient _http = new() { Timeout = TimeSpan.FromSeconds(6) };
    private static readonly JsonSerializerOptions Json = new() { PropertyNameCaseInsensitive = true };

    public IPAddress? Address { get; private set; }
    public string BaseUrl => Address is null ? "" : $"http://{Address}";

    /// <summary>Finds the radio by name (nightowl-radio.local via mDNS) or IP address.</summary>
    public async Task<RadioStatus?> ConnectAsync(string host, CancellationToken ct = default)
    {
        Address = null;
        host = host.Trim().Replace("http://", "").TrimEnd('/');
        if (!IPAddress.TryParse(host, out var ip))
        {
            var found = await Dns.GetHostAddressesAsync(host, AddressFamily.InterNetwork, ct);
            ip = found.FirstOrDefault() ?? throw new InvalidOperationException($"Could not find {host}");
        }
        Address = ip;
        return await StatusAsync(ct);
    }

    public Task<RadioStatus?> StatusAsync(CancellationToken ct = default) => GetAsync("/api/status", ct);

    public Task<RadioStatus?> GetAsync(string pathAndQuery, CancellationToken ct = default)
    {
        if (Address is null) return Task.FromResult<RadioStatus?>(null);
        return _http.GetFromJsonAsync<RadioStatus>(BaseUrl + pathAndQuery, Json, ct);
    }

    /// <summary>The laptop's own address on the network the radio is on.</summary>
    public IPAddress LocalAddressTowardRadio()
    {
        using var s = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        s.Connect(Address!, 80);                   // UDP connect sends nothing; it only picks the route
        return ((IPEndPoint)s.LocalEndPoint!).Address;
    }

    public static string Q(string s) => Uri.EscapeDataString(s);
}
