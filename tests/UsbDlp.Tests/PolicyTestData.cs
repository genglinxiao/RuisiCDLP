using UsbDlp.Policy;
using MatchType = UsbDlp.Policy.MatchType;

namespace UsbDlp.Tests;

internal static class PolicyTestData
{
    internal static readonly DateTimeOffset Now = new(2026, 8, 18, 12, 0, 0, TimeSpan.Zero);
    internal static readonly Guid WorkstationId = Guid.Parse("00000000-0000-0000-0000-000000000101");
    internal static readonly Guid GroupId = Guid.Parse("00000000-0000-0000-0000-000000000201");

    internal static DeviceInfo Device(
        string instanceId = @"USB\VID_0781&PID_5583\4C530001230904",
        string vid = "0781",
        string pid = "5583",
        string? serial = "4C530001230904",
        ProductDeviceClass deviceClass = ProductDeviceClass.UsbStorage,
        string classGuid = "{4d36e967-e325-11ce-bfc1-08002be10318}",
        IReadOnlyList<string>? ancestryClassGuids = null) =>
        new(
            instanceId,
            [@"USB\VID_0781&PID_5583"],
            [instanceId, @"USB\VID_0781&PID_5583"],
            vid,
            pid,
            serial,
            deviceClass,
            classGuid,
            "USBSTOR",
            "Test USB device",
            "usb",
            ["usb_storage"],
            ancestryClassGuids ?? []);

    internal static DeviceEntry Entry(
        MatchType matchType,
        EntryAction action,
        ScopeType scopeType = ScopeType.Global,
        Guid? scopeId = null,
        string? instanceId = null,
        string? vid = null,
        string? pid = null,
        string? serial = null,
        ProductDeviceClass? deviceClass = null,
        bool mandatory = false,
        DateTimeOffset? expiresAtUtc = null,
        Guid? id = null) =>
        new(
            id ?? Guid.NewGuid(),
            matchType,
            vid,
            pid,
            serial,
            instanceId,
            deviceClass,
            "Test entry",
            action,
            mandatory,
            scopeType,
            scopeId,
            expiresAtUtc,
            null,
            []);

    internal static CompiledPolicy Policy(
        GroupMode mode,
        params DeviceEntry[] entries) =>
        new(1, 42, Now, WorkstationId, GroupId, mode, entries);
}
