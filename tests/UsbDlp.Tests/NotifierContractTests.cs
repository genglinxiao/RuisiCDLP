using System.Text.Json;
using UsbDlp.Contracts;
using UsbDlp.Notifications;

namespace UsbDlp.Tests;

public sealed class NotifierContractTests
{
    [Fact]
    [Trait("TestId", "T-M5-01")]
    public void IpcMessageIsSingleLineAndRoundTrips()
    {
        DeviceSnapshot device = new(
            @"USB\VID_0781&PID_5567\SERIAL", [@"USB\VID_0781&PID_5567"],
            [@"USBSTOR\DISK&SAMPLE\SERIAL&0"], "0781", "5567", "SERIAL",
            "{4d36e967-e325-11ce-bfc1-08002be10318}", "disk", "Sample USB", "usb", ["usb_storage"]);
        NotifierMessage expected = new(1, "blocked", Guid.NewGuid(), device, "default_deny", null, null, null);

        string json = JsonSerializer.Serialize(expected, NotifierJson.Options);

        Assert.DoesNotContain('\r', json);
        Assert.DoesNotContain('\n', json);
        NotifierMessage actual = Assert.IsType<NotifierMessage>(JsonSerializer.Deserialize<NotifierMessage>(json, NotifierJson.Options));
        Assert.Equal(expected.EventId, actual.EventId);
        Assert.Equal(expected.Type, actual.Type);
        Assert.Equal(expected.ReasonCode, actual.ReasonCode);
        Assert.Equal(expected.Device!.InstanceId, actual.Device!.InstanceId);
        Assert.Equal(expected.Device.HardwareIds, actual.Device.HardwareIds);
        Assert.Equal(expected.Device.DeviceInstallIds, actual.Device.DeviceInstallIds);
    }
}
