using System.Text.RegularExpressions;

namespace UsbDlp.Windows;

public sealed record ParsedUsbIdentity(string Vid, string Pid, string? Serial);

public static partial class DeviceIdentityParser
{
    public static ParsedUsbIdentity? Parse(string instanceId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(instanceId);
        Match match = VidPidPattern().Match(instanceId);
        if (!match.Success)
        {
            return null;
        }

        string? serial = instanceId.Split('\\').LastOrDefault();
        if (string.IsNullOrWhiteSpace(serial) || serial.Contains('&', StringComparison.Ordinal))
        {
            serial = null;
        }

        return new(match.Groups["vid"].Value.ToUpperInvariant(), match.Groups["pid"].Value.ToUpperInvariant(), serial);
    }

    [GeneratedRegex(@"VID_(?<vid>[0-9A-Fa-f]{4})&PID_(?<pid>[0-9A-Fa-f]{4})", RegexOptions.CultureInvariant | RegexOptions.IgnoreCase)]
    private static partial Regex VidPidPattern();
}
