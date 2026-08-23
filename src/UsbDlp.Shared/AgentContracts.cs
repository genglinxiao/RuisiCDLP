using System.Text.Json;

namespace UsbDlp.Contracts;

public sealed record EnrollmentRequest(
    int SchemaVersion,
    string EnrollmentToken,
    string Hostname,
    string MachineSid,
    string AgentVersion,
    Guid ClientNonce);

public sealed record EnrollmentResponse(
    int SchemaVersion,
    Guid WorkstationId,
    string ApiCredential,
    DateTimeOffset ServerTimeUtc,
    int PolicyPollSeconds);

public sealed record DeviceSnapshot(
    string InstanceId,
    IReadOnlyList<string> HardwareIds,
    IReadOnlyList<string> DeviceInstallIds,
    string Vid,
    string Pid,
    string? Serial,
    string ClassGuid,
    string? Service,
    string FriendlyName,
    string Transport,
    IReadOnlyList<string> Capabilities);

public sealed record AgentEvent(
    Guid ClientEventId,
    string Type,
    DateTimeOffset OccurredAtUtc,
    string Hostname,
    string? UserSid,
    string? Username,
    DeviceSnapshot? Device,
    long PolicyVersion,
    Policy.Decision? Decision,
    Guid? MatchedEntryId,
    string? ReasonCode,
    string AgentVersion,
    JsonElement Details);

public sealed record EventBatchRequest(int SchemaVersion, IReadOnlyList<AgentEvent> Events);

public sealed record EventBatchResponse(
    int SchemaVersion,
    IReadOnlyList<Guid> AcceptedClientEventIds,
    IReadOnlyList<Guid> DuplicateClientEventIds,
    bool PolicyChanged,
    long CurrentPolicyVersion);

public sealed record AccessRequestCreate(
    int SchemaVersion,
    Guid ClientRequestId,
    DateTimeOffset OccurredAtUtc,
    string UserSid,
    string Username,
    DeviceSnapshot Device,
    int RequestedDurationDays,
    string Justification);

public sealed record AccessRequestResponse(
    int SchemaVersion,
    Guid RequestId,
    string Status,
    DateTimeOffset CreatedAtUtc,
    DateTimeOffset? RetryAfterUtc = null);

public enum AgentHealth
{
    Healthy,
    Degraded,
    FailClosed,
    ExternalPolicyConflict,
}

public sealed record HeartbeatRequest(
    int SchemaVersion,
    string AgentVersion,
    long AppliedPolicyVersion,
    AgentHealth Health,
    int QueueDepth,
    string? LastEnforcementErrorCode,
    DateTimeOffset SentAtUtc);

public sealed record HeartbeatResponse(
    int SchemaVersion,
    DateTimeOffset ServerTimeUtc,
    bool PolicyChanged,
    long CurrentPolicyVersion);
