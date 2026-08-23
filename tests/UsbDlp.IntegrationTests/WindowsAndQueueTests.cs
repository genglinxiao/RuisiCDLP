using UsbDlp.Agent;
using UsbDlp.Contracts;
using UsbDlp.Windows;
using UsbDlp.Policy;
using MatchType = UsbDlp.Policy.MatchType;

namespace UsbDlp.IntegrationTests;

public sealed class WindowsAndQueueTests
{
    [Fact]
    [Trait("TestId", "T-M3-01")]
    public void RegistryPlanUsesLayeredRemovablePolicyAndObservedAllowIds()
    {
        Guid workstation = Guid.NewGuid();
        DeviceEntry allow = new(
            Guid.NewGuid(), MatchType.InstanceId, null, null, null, @"USB\VID_0781&PID_5583\SERIAL", null,
            "Approved stick", EntryAction.Allow, false, ScopeType.Workstation, workstation,
            DateTimeOffset.UtcNow.AddDays(1), null, [@"USB\VID_0781&PID_5583\SERIAL", @"USB\VID_0781&PID_5583"]);
        CompiledPolicy policy = new(1, 2, DateTimeOffset.UtcNow, workstation, Guid.NewGuid(), GroupMode.DefaultDeny, [allow]);

        RegistryPolicyPlan plan = new RegistryPolicyManager().BuildPlan(policy, DateTimeOffset.UtcNow);

        Assert.Equal(1, plan.ScalarValues["AllowDenyLayered"]);
        Assert.Equal(1, plan.ScalarValues["DenyRemovableDevices"]);
        Assert.Equal(2, plan.AllowDeviceIds.Count);
        Assert.DoesNotContain(plan.ScalarValues.Keys, key => key.Contains("RemovableStorage", StringComparison.OrdinalIgnoreCase));
        Assert.DoesNotContain(plan.AllowDeviceIds.Values, value => value.Contains("{4d36e967", StringComparison.OrdinalIgnoreCase));
    }

    [Theory]
    [Trait("TestId", "T-M2-01")]
    [InlineData(@"USB\VID_0781&PID_5583\4C530001", "0781", "5583", "4C530001")]
    [InlineData(@"usb\vid_abcd&pid_ef01\SERIAL", "ABCD", "EF01", "SERIAL")]
    public void ParserExtractsCanonicalIdentity(string instanceId, string vid, string pid, string serial)
    {
        ParsedUsbIdentity parsed = Assert.IsType<ParsedUsbIdentity>(DeviceIdentityParser.Parse(instanceId));
        Assert.Equal(vid, parsed.Vid); Assert.Equal(pid, parsed.Pid); Assert.Equal(serial, parsed.Serial);
    }

    [Fact]
    [Trait("TestId", "T-M2-02")]
    public void ParserTreatsSubstitutedTokenAsMissingSerial() =>
        Assert.Null(DeviceIdentityParser.Parse(@"USB\VID_0781&PID_5583\6&ABC&0&2")!.Serial);

    [Fact]
    [Trait("TestId", "T-M3-02")]
    public void ActiveEnforcementTargetsStorageFunctionInsteadOfCompositeParent()
    {
        DeviceInfo device = PolicyTestDevice(
            @"USB\VID_0781&PID_5567\SERIAL",
            [@"USBSTOR\DISK&VEN_SANDISK\SERIAL&0", @"USB\VID_0781&PID_5567\SERIAL"]);

        Assert.Equal(@"USBSTOR\DISK&VEN_SANDISK\SERIAL&0", WindowsDeviceEnforcer.EnforcementTarget(device));
    }

    [Fact]
    [Trait("TestId", "T-M4-05")]
    public async Task DurableEventStoreSurvivesReopen()
    {
        string directory = Path.Combine(Path.GetTempPath(), $"usb-agent-tests-{Guid.NewGuid():N}");
        try
        {
            AgentStorage storage = new(new AgentOptions { DataDirectory = directory });
            DurableEventStore first = new(storage);
            await first.InitializeAsync(CancellationToken.None);
            AgentEvent item = new(Guid.NewGuid(), "agent_start", DateTimeOffset.UtcNow, "WS", null, null, null, 0, null, null, null, "1.0.0", System.Text.Json.JsonSerializer.SerializeToElement(new { }));
            await first.EnqueueAsync(item, CancellationToken.None);
            DurableEventStore reopened = new(storage);
            await reopened.InitializeAsync(CancellationToken.None);
            Assert.Contains(await reopened.PendingAsync(10, CancellationToken.None), value => value.ClientEventId == item.ClientEventId);
        }
        finally
        {
            if (Directory.Exists(directory)) Directory.Delete(directory, true);
        }
    }

    private static DeviceInfo PolicyTestDevice(string instanceId, IReadOnlyList<string> installIds) => new(
        instanceId, [@"USB\VID_0781&PID_5567"], installIds, "0781", "5567", "SERIAL",
        ProductDeviceClass.UsbStorage, "{4d36e967-e325-11ce-bfc1-08002be10318}", "disk",
        "SanDisk test device", "usb", ["usb_storage"], ["{4d36e967-e325-11ce-bfc1-08002be10318}"]);
}
