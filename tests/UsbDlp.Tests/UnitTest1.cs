namespace UsbDlp.Tests;

using System.Text.Json;
using UsbDlp.Contracts;
using UsbDlp.Policy;

public sealed class M0ScaffoldTests
{
    [Fact]
    [Trait("TestId", "T-M0-01")]
    public void TestRunnerIsWiredToFailTheBuild()
    {
        Assert.True(true);
    }
}

public sealed class DeviceMetadataContractTests
{
    [Fact]
    [Trait("TestId", "T-M2-10")]
    public void SnapshotMetadataRoundTripsAndRemainsBackwardCompatible()
    {
        DeviceSnapshot snapshot = new(@"USB\VID_1234&PID_5678\SERIAL", [], [], "1234", "5678", "SERIAL",
            "{guid}", "USBSTOR", "Transfer media", "usb", ["usb_storage"], "Acme", "SecureKey", "USB storage");
        DeviceSnapshot restored = JsonSerializer.Deserialize<DeviceSnapshot>(JsonSerializer.Serialize(snapshot, PolicyJson.Options), PolicyJson.Options)!;
        Assert.Equal(("Acme", "SecureKey", "USB storage"), (restored.Manufacturer, restored.Model, restored.Description));

        const string legacy = """{"instanceId":"USB\\VID_1234&PID_5678\\SERIAL","hardwareIds":[],"deviceInstallIds":[],"vid":"1234","pid":"5678","serial":"SERIAL","classGuid":"{guid}","service":"USBSTOR","friendlyName":"Legacy","transport":"usb","capabilities":["usb_storage"]}""";
        DeviceSnapshot oldSnapshot = JsonSerializer.Deserialize<DeviceSnapshot>(legacy, PolicyJson.Options)!;
        Assert.Null(oldSnapshot.Manufacturer);
        Assert.Null(oldSnapshot.Model);
        Assert.Null(oldSnapshot.Description);
    }
}
