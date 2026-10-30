namespace UsbDlp.Server;

internal static class BootstrapConfiguration
{
    public static void LoadFileSecrets(ConfigurationManager configuration)
    {
        Load(configuration, "UsbDlp:BootstrapAdminPassword", "UsbDlp:BootstrapAdminPasswordFile");
        Load(configuration, "UsbDlp:BootstrapEnrollmentToken", "UsbDlp:BootstrapEnrollmentTokenFile");
    }

    private static void Load(ConfigurationManager configuration, string valueKey, string fileKey)
    {
        if (!string.IsNullOrWhiteSpace(configuration[valueKey]))
        {
            return;
        }

        string? path = configuration[fileKey];
        if (string.IsNullOrWhiteSpace(path))
        {
            return;
        }

        if (!File.Exists(path))
        {
            throw new InvalidOperationException($"Configured secret file does not exist: {fileKey}.");
        }

        string value = File.ReadAllText(path).Trim();
        if (string.IsNullOrWhiteSpace(value))
        {
            throw new InvalidOperationException($"Configured secret file is empty: {fileKey}.");
        }

        configuration[valueKey] = value;
    }
}
