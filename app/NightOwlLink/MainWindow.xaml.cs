using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;
using NAudio.CoreAudioApi;
using NightOwlLink.Services;

namespace NightOwlLink;

public partial class MainWindow : Window
{
    private sealed record DeviceItem(string Id, string Name)
    {
        public override string ToString() => Name;   // what the closed combo box shows
    }

    // the radio's input buffer is kept around this level (seconds of audio):
    // enough to ride out WiFi hiccups, small enough to keep the delay low
    private const double TargetBufferS = 0.25;
    private const double OutputDelayS = 0.12;       // the radio's own decode and DMA buffers, roughly

    private readonly AppSettings _settings = AppSettings.Load();
    private readonly RadioClient _radio = new();
    private readonly MMDeviceEnumerator _devices = new();
    private readonly DispatcherTimer _poll = new() { Interval = TimeSpan.FromMilliseconds(500) };
    private readonly DispatcherTimer _meter = new() { Interval = TimeSpan.FromMilliseconds(33) };
    private readonly DispatcherTimer _volDebounce = new() { Interval = TimeSpan.FromMilliseconds(150) };
    private LoopbackStreamer? _streamer;
    private MMDevice? _captureDevice;
    private string? _startUrl;                       // set until the radio has acknowledged the stream
    private RadioStatus? _last;
    private bool _polling, _connected, _updatingVol;
    private string _localTitle = "", _sentTitle = "\u0001";
    private int _tick;
    private double _meterL, _meterR;
    private int _volBeforeMute = 12;

    public MainWindow()
    {
        InitializeComponent();
        HostBox.Text = _settings.RadioHost;
        AutoStartBox.IsChecked = _settings.AutoStart;
        TitlesBox.IsChecked = _settings.SendTitles;
        LoadDevices();

        _poll.Tick += async (_, _) => await PollAsync();
        _meter.Tick += (_, _) => UpdateMeters();
        _volDebounce.Tick += async (_, _) =>
        {
            _volDebounce.Stop();
            await CallAsync($"/api/vol?v={(int)VolSlider.Value}");
        };
        Loaded += async (_, _) =>
        {
            UseDarkTitleBar();
            await ConnectAsync();
            if (_connected && _settings.AutoStart) await StartStreamingAsync();
        };
    }

    // ---- devices ----
    private void LoadDevices()
    {
        var list = new List<DeviceItem>();
        string? defaultId = null;
        try { defaultId = _devices.GetDefaultAudioEndpoint(DataFlow.Render, Role.Multimedia).ID; } catch { }
        foreach (var d in _devices.EnumerateAudioEndPoints(DataFlow.Render, DeviceState.Active))
            list.Add(new DeviceItem(d.ID, d.ID == defaultId ? $"{d.FriendlyName}  (default)" : d.FriendlyName));
        DeviceBox.ItemsSource = list;
        DeviceBox.SelectedItem = list.FirstOrDefault(d => d.Id == _settings.DeviceId)
                              ?? list.FirstOrDefault(d => d.Id == defaultId)
                              ?? list.FirstOrDefault();
    }

    private async void Device_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (DeviceBox.SelectedItem is not DeviceItem d) return;
        _settings.DeviceId = d.Id;
        _settings.Save();
        if (_streamer?.IsRunning == true)          // switch capture on the fly
        {
            await StopStreamingAsync(tellRadio: false);
            await StartStreamingAsync();
        }
    }

    // ---- radio connection ----
    private async void Connect_Click(object sender, RoutedEventArgs e) => await ConnectAsync();

    private async Task ConnectAsync()
    {
        ConnectBtn.IsEnabled = false;
        SetState("searching", Brush("Amber"));
        Log($"Looking for {HostBox.Text}…");
        try
        {
            var s = await _radio.ConnectAsync(HostBox.Text);
            _connected = s is not null;
            _settings.RadioHost = HostBox.Text.Trim();
            _settings.Save();
            Log($"Connected to the radio at {_radio.Address}.");
            Render(s);
            _poll.Start();
        }
        catch (Exception ex)
        {
            _connected = false;
            SetState("offline", Brush("Faint"));
            RadioStation.Text = "Radio not found";
            RadioTitle.Text = "Check it is on and on the same WiFi, or type its IP address";
            Log($"Could not reach {HostBox.Text}: {ex.Message}");
        }
        finally
        {
            ConnectBtn.IsEnabled = true;
            StreamBtn.IsEnabled = _connected;
            VolSlider.IsEnabled = MuteBtn.IsEnabled = _connected;
        }
    }

    private async Task PollAsync()
    {
        if (_polling || !_connected) return;
        _polling = true;
        try
        {
            _tick++;
            bool streaming = _streamer?.IsRunning == true;
            if (_tick % 4 == 0 || _localTitle.Length == 0)
                _localTitle = await NowPlaying.GetAsync();
            LocalTitle.Text = _localTitle.Length > 0 ? _localTitle : "—";

            RadioStatus? s;
            if (streaming && _startUrl is not null)
            {
                await AnnounceAsync();                 // the first announcement did not get through
                s = _last;
            }
            else if (streaming)
            {
                // the ping keeps the Laptop source alive on the radio; send the title only when it changes
                string title = TitlesBox.IsChecked == true ? _localTitle : "";
                string q = title != _sentTitle ? $"?title={RadioClient.Q(title)}" : "";
                s = await _radio.GetAsync("/api/laptop/ping" + q);
                if (q.Length > 0) _sentTitle = title;
                SteerBuffer(s);
            }
            else
            {
                s = await _radio.StatusAsync();
            }
            Render(s);
        }
        catch (Exception ex)
        {
            SetState("no reply", Brush("Red"));
            if (_tick % 10 == 0) Log($"Radio not answering: {ex.Message}");
        }
        finally
        {
            _polling = false;
        }
    }

    // keeps the radio's buffer near the target by sending slightly faster or slower
    private void SteerBuffer(RadioStatus? s)
    {
        if (_streamer is null || s is null) return;
        if (!s.IsLaptop || !_streamer.RadioConnected) { _streamer.RateTrim = 0; DelayText.Text = "--"; return; }
        double buffered = s.InputBuffer / (double)LoopbackStreamer.BytesPerSecond;
        double error = TargetBufferS - buffered;
        // gentle proportional trim, capped at ±0.5 %; inside ±40 ms leave it alone
        _streamer.RateTrim = Math.Abs(error) < 0.04 ? 0 : Math.Clamp(error * 0.02, -0.005, 0.005);
        DelayText.Text = $"{buffered + OutputDelayS:0.0} s";
    }

    private void Render(RadioStatus? s)
    {
        if (s is null) return;
        _last = s;
        RadioStation.Text = string.IsNullOrWhiteSpace(s.Station) ? "Night Owl radio" : s.Station;
        RadioTitle.Text = s.IsLaptop
            ? (s.Active ? "Playing your laptop" : "Connecting to your laptop…")
            : (string.IsNullOrWhiteSpace(s.Title) ? (s.Source == "sd" ? "SD card" : "Radio") : s.Title);
        RadioClock.Text = string.IsNullOrEmpty(s.Time) ? "--:--" : s.Time;

        if (!VolSlider.IsMouseCaptureWithin && !_volDebounce.IsEnabled)
        {
            _updatingVol = true;
            VolSlider.Value = s.Volume;
            _updatingVol = false;
        }
        VolText.Text = s.Volume == 0 ? "off" : s.Volume.ToString();
        MuteBtn.Content = s.Volume == 0 ? "Unmute" : "Mute";

        bool streaming = _streamer?.IsRunning == true;
        if (streaming && s.IsLaptop && s.Active && _streamer!.RadioConnected) SetState("on the radio", Brush("Green"));
        else if (streaming) SetState("waiting for radio", Brush("Amber"));
        else SetState(s.Paused ? "radio paused" : "connected", Brush("Cyan"));
    }

    // ---- streaming ----
    private async void Stream_Click(object sender, RoutedEventArgs e)
    {
        StreamBtn.IsEnabled = false;
        if (_streamer?.IsRunning == true) await StopStreamingAsync(tellRadio: true);
        else await StartStreamingAsync();
        StreamBtn.IsEnabled = _connected;
    }

    private async Task StartStreamingAsync()
    {
        if (DeviceBox.SelectedItem is not DeviceItem d) { Log("No playback device to capture."); return; }
        string url;
        try
        {
            _streamer ??= CreateStreamer();
            _captureDevice?.Dispose();
            _captureDevice = _devices.GetDevice(d.Id);   // kept for as long as the capture runs
            _streamer.Start(_captureDevice);
            url = $"http://{_radio.LocalAddressTowardRadio()}:{_streamer.Port}{LoopbackStreamer.StreamPath}";
            _meter.Start();
            SetStreamingLook(true);
        }
        catch (Exception ex)
        {
            Log($"Could not start: {ex.Message}");
            _streamer?.Stop();
            SetStreamingLook(false);
            return;
        }

        // tell the radio where the stream is; if it is slow to answer the stream keeps
        // running, and the radio still picks it up from the next ping
        _startUrl = url;
        await AnnounceAsync();
    }

    private async Task AnnounceAsync()
    {
        if (_startUrl is null) return;
        string title = TitlesBox.IsChecked == true ? await NowPlaying.GetAsync() : "";
        try
        {
            Render(await _radio.GetAsync($"/api/laptop?url={RadioClient.Q(_startUrl)}&title={RadioClient.Q(title)}&go=1"));
            _sentTitle = title;
            Log($"Streaming to the radio from {_startUrl}");
            _startUrl = null;
        }
        catch (Exception ex)
        {
            Log($"Streaming; waiting for the radio to answer ({ex.Message})");
        }
    }

    private async Task StopStreamingAsync(bool tellRadio)
    {
        _streamer?.Stop();
        _captureDevice?.Dispose();
        _captureDevice = null;
        _startUrl = null;
        _meter.Stop();
        _meterL = _meterR = 0;
        MeterL.Width = MeterR.Width = 0;
        DelayText.Text = "--";
        SetStreamingLook(false);
        if (tellRadio)
        {
            await CallAsync("/api/laptop/stop");
            Log("Stopped. The radio went back to its own station.");
        }
    }

    private LoopbackStreamer CreateStreamer()
    {
        var s = new LoopbackStreamer(_settings.Port);
        s.Log += msg => Dispatcher.BeginInvoke(() => Log(msg));
        return s;
    }

    private void SetStreamingLook(bool on)
    {
        StreamText.Text = on ? "Stop" : "Play laptop on the radio";
        StreamIcon.Data = Geometry.Parse(on ? "M0,0 H12 V12 H0 Z" : "M0,0 L12,7 L0,14 Z");
        StreamIcon.Fill = on ? Brush("Red") : Brush("Green");
        StreamBtn.BorderBrush = on ? new SolidColorBrush(Color.FromArgb(0x66, 0xF8, 0x71, 0x71)) : Brushes.Transparent;
    }

    private void UpdateMeters()
    {
        if (_streamer is null) return;
        var (l, r) = _streamer.TakePeaks();
        // fast attack, slow fall, on a dB-ish scale so quiet music still moves
        static double Shape(float p) => p <= 0.0001f ? 0 : Math.Clamp(1 + Math.Log10(p) / 2.4, 0, 1);
        _meterL = Math.Max(Shape(l), _meterL * 0.86);
        _meterR = Math.Max(Shape(r), _meterR * 0.86);
        double w = MeterTrackL.ActualWidth;
        MeterL.Width = w * _meterL;
        MeterR.Width = w * _meterR;
        SentText.Text = $"{_streamer.BytesSent / 1048576.0:0.0} MB";
    }

    // ---- volume ----
    private void Vol_Changed(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_updatingVol || !_connected) return;
        VolText.Text = e.NewValue == 0 ? "off" : ((int)e.NewValue).ToString();
        _volDebounce.Stop();
        _volDebounce.Start();
    }

    private async void Mute_Click(object sender, RoutedEventArgs e)
    {
        int v = _last?.Volume ?? 0;
        if (v > 0) { _volBeforeMute = v; await CallAsync("/api/vol?v=0"); }
        else await CallAsync($"/api/vol?v={_volBeforeMute}");
    }

    private void Option_Click(object sender, RoutedEventArgs e)
    {
        _settings.AutoStart = AutoStartBox.IsChecked == true;
        _settings.SendTitles = TitlesBox.IsChecked == true;
        _settings.Save();
        _sentTitle = "\u0001";                     // resend (or clear) the title on the next ping
    }

    // ---- helpers ----
    private async Task CallAsync(string path)
    {
        try { Render(await _radio.GetAsync(path)); }
        catch (Exception ex) { Log($"Radio did not answer: {ex.Message}"); }
    }

    private void SetState(string text, Brush color)
    {
        StateText.Text = text;
        StateDot.Fill = color;
    }

    private Brush Brush(string key) => (Brush)FindResource(key);

    private void Log(string msg) => LogText.Text = msg;

    protected override async void OnClosing(CancelEventArgs e)
    {
        if (_streamer?.IsRunning == true)
        {
            e.Cancel = true;                        // tell the radio first, then close
            await StopStreamingAsync(tellRadio: true);
            _streamer.Dispose();
            _streamer = null;
            Close();
            return;
        }
        base.OnClosing(e);
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    private void UseDarkTitleBar()
    {
        int on = 1;
        DwmSetWindowAttribute(new WindowInteropHelper(this).Handle, 20, ref on, sizeof(int));  // DWMWA_USE_IMMERSIVE_DARK_MODE
    }
}
