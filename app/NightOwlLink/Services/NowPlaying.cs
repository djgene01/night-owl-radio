using Windows.Media.Control;

namespace NightOwlLink.Services;

/// <summary>
/// Reads "Artist - Title" from whatever app Windows shows in its media overlay
/// (Spotify, browsers, Media Player...), so the radio's OLED can show it.
/// </summary>
public static class NowPlaying
{
    private static GlobalSystemMediaTransportControlsSessionManager? _manager;

    public static async Task<string> GetAsync()
    {
        try
        {
            _manager ??= await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
            var session = _manager.GetCurrentSession();
            if (session is null) return "";
            var props = await session.TryGetMediaPropertiesAsync();
            string artist = props.Artist?.Trim() ?? "", title = props.Title?.Trim() ?? "";
            if (artist.Length > 0 && title.Length > 0) return $"{artist} - {title}";
            return title;
        }
        catch
        {
            return "";                // media sessions are best effort
        }
    }
}
