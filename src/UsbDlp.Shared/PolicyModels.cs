namespace UsbDlp.Policy;

public sealed record DeviceInfo(
    string InstanceId,
    IReadOnlyList<string> HardwareIds,
    IReadOnlyList<string> DeviceInstallIds,
    string Vid,
    string Pid,
    string? Serial,
    ProductDeviceClass DeviceClass,
    string ClassGuid,
    string? Service,
    string FriendlyName,
    string Transport,
    IReadOnlyList<string> Capabilities,
    IReadOnlyList<string> AncestryClassGuids);

public sealed record DeviceEntry(
    Guid Id,
    MatchType MatchType,
    string? Vid,
    string? Pid,
    string? Serial,
    string? InstanceId,
    ProductDeviceClass? DeviceClass,
    string DisplayName,
    EntryAction Action,
    bool Mandatory,
    ScopeType ScopeType,
    Guid? ScopeId,
    DateTimeOffset? ExpiresAtUtc,
    string? Note,
    IReadOnlyList<string> DeviceInstallIds);

public sealed record CompiledPolicy(
    int SchemaVersion,
    long PolicyVersion,
    DateTimeOffset GeneratedAtUtc,
    Guid WorkstationId,
    Guid GroupId,
    GroupMode GroupMode,
    IReadOnlyList<DeviceEntry> Entries);

public sealed record PolicyEvaluation(
    Decision Decision,
    Guid? MatchedEntryId,
    string ReasonCode,
    long PolicyVersion,
    DateTimeOffset EvaluatedAtUtc);

public sealed record PolicyValidationError(string Code, string Message, Guid? EntryId = null);

