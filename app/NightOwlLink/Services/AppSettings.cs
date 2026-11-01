using System.IO;
using System.Text.Json;

namespace NightOwlLink.Services;

/// <summary>Remembered between runs, in %AppData%\NightOwlLink\settings.json.</summary>
public sealed class AppSettings
{
    public string RadioHost { get; set; } = "nightowl-radio.local";
    public int Port { get; set; } = 8090;
    public string? DeviceId { get; set; }
    public bool AutoStart { get; set; }
    public bool SendTitles { get; set; } = true;

    private static string FilePath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "NightOwlLink", "settings.json");

    public static AppSettings Load()
    {
        try
        {
            if (File.Exists(FilePath))
                return JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(FilePath)) ?? new();
        }
        catch { /* a broken file just means defaults */ }
        return new();
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            File.WriteAllText(FilePath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch { }
    }
}
