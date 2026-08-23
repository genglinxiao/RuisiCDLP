using System.Text.Json;
using UsbDlp.Policy;
using UsbDlp.Windows;

namespace UsbDlp.Agent;

public sealed class ConfiguredDeviceEnumerator(AgentOptions options, WindowsDeviceEnumerator windowsEnumerator) : IWindowsDeviceEnumerator
{
    public IReadOnlyList<DeviceInfo> EnumerateInScopeDevices()
    {
        if (string.IsNullOrWhiteSpace(options.SimulatedDevicePath))
            return windowsEnumerator.EnumerateInScopeDevices();
        using FileStream stream = File.OpenRead(Path.GetFullPath(options.SimulatedDevicePath));
        DeviceInfo device = JsonSerializer.Deserialize<DeviceInfo>(stream, PolicyJson.Options) ?? throw new JsonException("Simulated device JSON is null.");
        return [device];
    }
}
