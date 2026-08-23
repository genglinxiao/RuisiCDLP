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
    string Health,
    string? Description = null);

public sealed record PagedResult<T>(IReadOnlyList<T> Items, int Page, int PageSize, int TotalCount, string Query);

public sealed record DeviceInventoryRow(
    Guid Id,
    string IdentityConfidence,
    string Vid,
    string Pid,
    string? Serial,
    string FriendlyName,
    string? Manufacturer,
    string? Model,
    string? Description,
    DateTimeOffset FirstSeenUtc,
    DateTimeOffset LastSeenUtc,
    int WorkstationCount);

public sealed record WorkstationInventoryRow(
    Guid Id,
    string Hostname,
    string MachineSid,
    string GroupName,
    string? Description,
    string AgentVersion,
    DateTimeOffset? LastSeenUtc,
    string Health,
    int DeviceCount);

public sealed record WorkstationDeviceRow(
    Guid WorkstationId,
    Guid DeviceId,
    string Hostname,
    string DeviceName,
    string InstanceId,
    string EffectiveDecision,
    string PolicySource,
    bool CanRevoke,
    Guid? MatchedEntryId,
    string ReasonCode,
    DateTimeOffset? ExpiresAtUtc,
    long PolicyVersion,
    string? RequestQualification,
    string ObservedEnforcement,
    DateTimeOffset? LastObservedAtUtc,
    string Freshness,
    string? LastEventType,
    IReadOnlyList<RequestRow> RecentRequests);

public sealed record DeviceDetail(DeviceInventoryRow Device, IReadOnlyList<WorkstationDeviceRow> Workstations);
public sealed record WorkstationDetail(WorkstationInventoryRow Workstation, IReadOnlyList<WorkstationDeviceRow> Devices);
public sealed record InventorySearchResult(
    IReadOnlyList<DeviceInventoryRow> Devices,
    IReadOnlyList<WorkstationInventoryRow> Workstations,
    int Page,
    int PageSize,
    string Query);

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
public sealed record GrantRequest(DateTimeOffset? ExpiresAtUtc, string Reason);
public sealed record GrantResponse(Guid EntryId, long PolicyVersion, WorkstationDeviceRow Pair);
public sealed record DescriptionRequest(string? Description);
