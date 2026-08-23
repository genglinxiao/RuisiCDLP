namespace UsbDlp.Agent;

public sealed class AgentOptions
{
    public const string SectionName = "UsbDlp";
    public string ServerBaseUrl { get; set; } = "http://127.0.0.1:5091";
    public string EnrollmentToken { get; set; } = string.Empty;
    public string? EnrollmentTokenFile { get; set; }
    public string DataDirectory { get; set; } = "App_Data";
    public bool DryRun { get; set; } = true;
    public bool AllowInsecureHttp { get; set; } = true;
    public int LocalPollSeconds { get; set; } = 10;
    public string? SimulatedDevicePath { get; set; }
    public string? NotifierExecutablePath { get; set; }
    public string[] CertificateSha256Pins { get; set; } = [];
}
