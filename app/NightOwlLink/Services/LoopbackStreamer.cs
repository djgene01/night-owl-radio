using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using NAudio.CoreAudioApi;
using NAudio.Lame;
using NAudio.Wave;
using NAudio.Wave.SampleProviders;

namespace NightOwlLink.Services;

/// <summary>
/// Captures what the laptop is playing (WASAPI loopback), converts it to 44.1 kHz
/// stereo, encodes 320 kbps MP3 and serves it as an endless stream on
/// http://&lt;laptop&gt;:port/stream.mp3. The radio plays that URL exactly like a web
/// radio station. (Plain WAV would be simpler, but the radio's audio library does
/// not read WAV headers on web streams.)
///
/// The stream is paced by a stopwatch, never by the capture: when nothing is playing
/// Windows delivers no loopback data, and silence is sent instead, so the radio
/// never runs dry. <see cref="RateTrim"/> nudges the send rate a few tenths of a
/// percent so the radio's buffer stays near its target despite clock drift.
/// </summary>
public sealed class LoopbackStreamer : IDisposable
{
    public const int SampleRate = 44100;
    public const int Channels = 2;
    public const int Kbps = 320;
    public const int BytesPerSecond = Kbps * 1000 / 8;   // of the MP3 stream the radio buffers
    public const string StreamPath = "/stream.mp3";

    // keep at most this much captured audio waiting; older audio is dropped so the
    // delay to the radio cannot creep up
    private static readonly TimeSpan MaxBacklog = TimeSpan.FromMilliseconds(150);

    private readonly object _clientLock = new();
    private WasapiLoopbackCapture? _capture;
    private BufferedWaveProvider? _buffered;
    private ISampleProvider? _source;
    private TcpListener? _listener;
    private TcpClient? _client;
    private LameMP3FileWriter? _encoder;           // one per radio connection, writes into its socket
    private CancellationTokenSource? _cts;
    private Thread? _pump;
    private float _peakL, _peakR;

    public LoopbackStreamer(int port) => Port = port;

    public int Port { get; }
    public bool IsRunning => _cts is not null;
    public bool RadioConnected { get { lock (_clientLock) return _client is not null; } }
    public string DeviceName { get; private set; } = "";

    /// <summary>Send-rate adjustment, e.g. +0.004 sends 0.4 % faster. Set from the radio's buffer level.</summary>
    public double RateTrim { get; set; }

    public long BytesSent { get; private set; }

    public event Action<string>? Log;

    public void Start(MMDevice device)
    {
        if (IsRunning) Stop();

        DeviceName = device.FriendlyName;
        _capture = new WasapiLoopbackCapture(device);
        _buffered = new BufferedWaveProvider(_capture.WaveFormat)
        {
            BufferDuration = TimeSpan.FromSeconds(1),
            DiscardOnBufferOverflow = true,
            ReadFully = true,          // empty buffer reads as silence
        };
        _capture.DataAvailable += (_, e) => _buffered.AddSamples(e.Buffer, 0, e.BytesRecorded);
        _capture.RecordingStopped += (_, e) =>
        {
            if (e.Exception is not null) Log?.Invoke($"Capture stopped: {e.Exception.Message}");
        };
        _source = BuildChain(_buffered);

        _listener = new TcpListener(IPAddress.Any, Port);
        _listener.Start();

        _cts = new CancellationTokenSource();
        _ = AcceptLoop(_cts.Token);
        _pump = new Thread(() => PumpLoop(_cts.Token)) { IsBackground = true, Priority = ThreadPriority.AboveNormal, Name = "stream pump" };
        _pump.Start();
        _capture.StartRecording();

        var f = _capture.WaveFormat;
        Log?.Invoke($"Capturing \"{DeviceName}\" ({f.SampleRate} Hz, {f.Channels} ch), serving port {Port}");
    }

    public void Stop()
    {
        _cts?.Cancel();
        try { _capture?.StopRecording(); } catch { /* device may be gone */ }
        _capture?.Dispose();
        _capture = null;
        try { _listener?.Stop(); } catch { }
        _listener = null;
        DropClient();
        _pump?.Join(500);
        _pump = null;
        _cts?.Dispose();
        _cts = null;
        BytesSent = 0;
    }

    /// <summary>Peak levels (0..1) since the last call, for the level meter.</summary>
    public (float Left, float Right) TakePeaks()
    {
        var p = (_peakL, _peakR);
        _peakL = _peakR = 0;
        return p;
    }

    public void Dispose() => Stop();

    // ---- audio chain: any channel count / rate -> stereo float at 44.1 kHz ----
    private static ISampleProvider BuildChain(IWaveProvider captured)
    {
        ISampleProvider s = captured.ToSampleProvider();
        if (s.WaveFormat.Channels == 1)
        {
            s = new MonoToStereoSampleProvider(s);
        }
        else if (s.WaveFormat.Channels > 2)
        {
            var mux = new MultiplexingSampleProvider(new[] { s }, 2);   // front left / right
            mux.ConnectInputToOutput(0, 0);
            mux.ConnectInputToOutput(1, 1);
            s = mux;
        }
        if (s.WaveFormat.SampleRate != SampleRate)
            s = new WdlResamplingSampleProvider(s, SampleRate);
        return s;
    }

    // ---- HTTP: one radio at a time; a new connection replaces the old one ----
    private async Task AcceptLoop(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested && _listener is not null)
        {
            TcpClient client;
            try { client = await _listener.AcceptTcpClientAsync(ct); }
            catch { break; }

            try
            {
                client.NoDelay = true;
                client.SendTimeout = 3000;
                var ns = client.GetStream();
                string request = await ReadRequestHead(ns, ct);
                string path = request.Split(' ').ElementAtOrDefault(1) ?? "";
                if (!request.StartsWith("GET ") || !path.StartsWith(StreamPath))
                {
                    await ns.WriteAsync(Encoding.ASCII.GetBytes("HTTP/1.0 404 Not Found\r\nConnection: close\r\n\r\n"), ct);
                    client.Dispose();
                    continue;
                }

                var head = Encoding.ASCII.GetBytes(
                    "HTTP/1.0 200 OK\r\n" +
                    "Content-Type: audio/mpeg\r\n" +
                    "Cache-Control: no-cache\r\n" +
                    "Connection: close\r\n" +
                    "icy-name: Laptop\r\n" +
                    $"icy-br: {Kbps}\r\n" +
                    "Server: NightOwlLink\r\n\r\n");
                await ns.WriteAsync(head, ct);

                var encoder = new LameMP3FileWriter(new SocketSink(ns), new WaveFormat(SampleRate, 16, Channels), Kbps);
                lock (_clientLock)
                {
                    DropClientLocked();
                    _client = client;
                    _encoder = encoder;
                }
                Log?.Invoke($"Radio connected from {((IPEndPoint)client.Client.RemoteEndPoint!).Address}");
            }
            catch (Exception ex) when (ex is IOException or SocketException or OperationCanceledException)
            {
                client.Dispose();
            }
        }
    }

    private static async Task<string> ReadRequestHead(NetworkStream ns, CancellationToken ct)
    {
        var sb = new StringBuilder();
        var buf = new byte[1];
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeout.CancelAfter(3000);
        while (sb.Length < 4096)
        {
            int n = await ns.ReadAsync(buf, timeout.Token);
            if (n == 0) break;
            sb.Append((char)buf[0]);
            if (sb.Length >= 4 && sb[^1] == '\n' && sb[^2] == '\r' && sb[^3] == '\n' && sb[^4] == '\r') break;
        }
        return sb.ToString();
    }

    private void DropClient()
    {
        lock (_clientLock) DropClientLocked();
    }

    private void DropClientLocked()
    {
        var enc = _encoder;
        _encoder = null;
        _client?.Dispose();                        // close the socket first, so the final flush cannot block
        _client = null;
        try { enc?.Dispose(); } catch { /* flushing into a closed socket */ }
    }

    /// <summary>Write-only stream that hands the encoder's MP3 frames straight to the socket.</summary>
    private sealed class SocketSink(NetworkStream ns) : Stream
    {
        public override bool CanRead => false;
        public override bool CanSeek => false;
        public override bool CanWrite => true;
        public override long Length => throw new NotSupportedException();
        public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
        public override void Write(byte[] buffer, int offset, int count) => ns.Write(buffer, offset, count);
        public override void Flush() => ns.Flush();
        public override int Read(byte[] buffer, int offset, int count) => throw new NotSupportedException();
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
    }

    // ---- real-time pump ----
    private void PumpLoop(CancellationToken ct)
    {
        const int maxChunkFrames = SampleRate / 20;        // 50 ms
        var floats = new float[maxChunkFrames * Channels];
        var bytes = new byte[maxChunkFrames * Channels * 2];
        var sw = Stopwatch.StartNew();
        double due = 0;                                     // frames that should have gone out
        long sent = 0;
        double last = 0;
        bool wasConnected = false;

        while (!ct.IsCancellationRequested)
        {
            LameMP3FileWriter? enc;
            lock (_clientLock) enc = _encoder;

            if (enc is null || _source is null || _buffered is null)
            {
                // nobody listening: keep the capture from piling up, restart the clock
                if (wasConnected) Log?.Invoke("Radio disconnected");
                wasConnected = false;
                _buffered?.ClearBuffer();
                sw.Restart();
                due = 0; sent = 0; last = 0;
                Thread.Sleep(20);
                continue;
            }
            wasConnected = true;

            double now = sw.Elapsed.TotalSeconds;
            due += (now - last) * SampleRate * (1.0 + Math.Clamp(RateTrim, -0.01, 0.01));
            last = now;

            int frames = (int)Math.Min(maxChunkFrames, (long)due - sent);
            if (frames < SampleRate / 200)                  // under 5 ms due: wait
            {
                Thread.Sleep(4);
                continue;
            }

            if (_buffered.BufferedDuration > MaxBacklog)    // capture got ahead: drop the oldest audio
            {
                int skip = (int)((_buffered.BufferedDuration - TimeSpan.FromMilliseconds(40)).TotalSeconds * SampleRate);
                while (skip > 0)
                {
                    int n = Math.Min(skip, maxChunkFrames);
                    _source.Read(floats, 0, n * Channels);
                    skip -= n;
                }
            }

            int got = _source.Read(floats, 0, frames * Channels);
            if (got < frames * Channels) Array.Clear(floats, got, frames * Channels - got);

            float pl = 0, pr = 0;
            for (int i = 0, j = 0; i < frames * Channels; i += 2, j += 4)
            {
                float l = Math.Clamp(floats[i], -1f, 1f), r = Math.Clamp(floats[i + 1], -1f, 1f);
                pl = Math.Max(pl, Math.Abs(l));
                pr = Math.Max(pr, Math.Abs(r));
                short sl = (short)(l * 32767f), sr = (short)(r * 32767f);
                bytes[j] = (byte)sl; bytes[j + 1] = (byte)(sl >> 8);
                bytes[j + 2] = (byte)sr; bytes[j + 3] = (byte)(sr >> 8);
            }
            _peakL = Math.Max(_peakL, pl);
            _peakR = Math.Max(_peakR, pr);

            try
            {
                enc.Write(bytes, 0, frames * Channels * 2);   // encodes and sends whole MP3 frames
                sent += frames;
                BytesSent += frames * (long)BytesPerSecond / SampleRate;   // MP3 bytes, near enough
            }
            catch (Exception ex) when (ex is IOException or ObjectDisposedException or InvalidOperationException)
            {
                DropClient();
            }
        }
    }
}
