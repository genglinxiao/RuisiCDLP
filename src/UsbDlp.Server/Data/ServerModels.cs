using UsbDlp.Policy;
using MatchType = UsbDlp.Policy.MatchType;

namespace UsbDlp.Server.Data;

public sealed record AuthenticatedAgent(
    Guid WorkstationId,
    Guid GroupId,
    string Hostname,
    long AppliedPolicyVersion);

public sealed record DashboardModel(
    long PolicyVersion,
    int WorkstationCount,
    int OnlineCount,
    int BlockedToday,
    int PendingRequests,
    IReadOnlyList<WorkstationRow> Workstations,
    IReadOnlyList<PolicyRow> Policies,
    IReadOnlyList<RequestRow> Requests,
    IReadOnlyList<EventRow> Events,
    IReadOnlyList<AuditRow> Audit);

public sealed record WorkstationRow(
    Guid Id,
    string Hostname,
    string MachineSid,
    string GroupName,
    string AgentVersion,
    DateTimeOffset? LastSeenUtc,
    long AppliedPolicyVersion,
    string Health);

public sealed record PolicyRow(
    Guid Id,
    string DisplayName,
    MatchType MatchType,
    string MatchValue,
    EntryAction Action,
    ScopeType ScopeType,
    Guid? ScopeId,
    bool Mandatory,
    DateTimeOffset? ExpiresAtUtc);

public sealed record RequestRow(
    Guid Id,
    Guid WorkstationId,
    string Hostname,
    string Username,
    string DeviceName,
    string InstanceId,
    int RequestedDurationDays,
    string Justification,
    string Status,
    DateTimeOffset CreatedAtUtc);

public sealed record EventRow(
    Guid Id,
    string Type,
    DateTimeOffset OccurredAtUtc,
    string Hostname,
    string? Username,
    string? DeviceName,
    string? InstanceId,
    string? Decision,
    string? ReasonCode);

public sealed record AuditRow(
    Guid Id,
    string Actor,
    string Action,
    string TargetType,
    string TargetId,
    DateTimeOffset OccurredAtUtc);

public sealed record AddPolicyRequest(
    string DisplayName,
    MatchType MatchType,
    string? Vid,
    string? Pid,
    string? Serial,
    string? InstanceId,
    ProductDeviceClass? DeviceClass,
    EntryAction Action,
    ScopeType ScopeType,
    Guid? ScopeId,
    bool Mandatory,
    DateTimeOffset? ExpiresAtUtc);

public sealed record DecisionRequest(int DurationDays, string? Reason);
