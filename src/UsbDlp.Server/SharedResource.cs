using System.Globalization;
using System.Collections;
using System.Resources;

namespace UsbDlp.Server;

public sealed class SharedResource
{
    private static readonly Lazy<Dictionary<string, string>> Chinese = new(LoadChinese);
    public string this[string key] => CultureInfo.CurrentUICulture.Name == "zh-CN" && Chinese.Value.TryGetValue(key, out string? value) ? value : key;

    private static Dictionary<string, string> LoadChinese()
    {
        CultureInfo culture = CultureInfo.GetCultureInfo("zh-CN");
        System.Reflection.Assembly satellite = typeof(SharedResource).Assembly.GetSatelliteAssembly(culture);
        using Stream stream = satellite.GetManifestResourceStream("UsbDlp.Server.Resources.SharedResource.resources")
            ?? throw new MissingManifestResourceException("The zh-CN administration resources are missing.");
        using ResourceReader reader = new(stream);
        Dictionary<string, string> values = new(StringComparer.Ordinal);
        IDictionaryEnumerator entries = reader.GetEnumerator();
        while (entries.MoveNext())
            if (entries.Key is string key && entries.Value is string value) values[key] = value;
        return values;
    }
}
