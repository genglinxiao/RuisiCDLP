using UsbDlp.Contracts;
using UsbDlp.Policy;
using System.Text.Json;

namespace UsbDlp.Notifications;

public sealed record NotifierMessage(
    int SchemaVersion,
    string Type,
    Guid EventId,
    DeviceSnapshot? Device,
    string? ReasonCode,
    Guid? RequestId,
    string? Status,
    string? Error,
    DateTimeOffset? RetryAfterUtc = null);

public sealed record NotifierAccessRequest(
    int SchemaVersion,
    string Type,
    Guid EventId,
    DeviceSnapshot Device,
    int RequestedDurationDays,
    string Justification);

public static class NotifierJson
{
    public static JsonSerializerOptions Options { get; } = new(PolicyJson.Options)
    {
        WriteIndented = false,
    };
}
