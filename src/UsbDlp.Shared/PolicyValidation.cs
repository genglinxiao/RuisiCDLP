using System.Text.RegularExpressions;

namespace UsbDlp.Policy;

public sealed class PolicyValidationException : Exception
{
    public PolicyValidationException()
        : this("Policy validation failed.", [])
    {
    }

    public PolicyValidationException(string message)
        : this(message, [])
    {
    }

    public PolicyValidationException(string message, Exception innerException)
        : base(message, innerException)
    {
        Errors = [];
    }

    public PolicyValidationException(IReadOnlyList<PolicyValidationError> errors)
        : this(
            $"Policy validation failed: {string.Join(", ", errors.Select(error => error.Code))}.",
            errors)
    {
    }

    private PolicyValidationException(string message, IReadOnlyList<PolicyValidationError> errors)
        : base(message)
    {
        Errors = errors;
    }

    public IReadOnlyList<PolicyValidationError> Errors { get; }
}

public static partial class CompiledPolicyValidator
{
    private const int MaxInstanceIdLength = 200;
    private const int MaxSerialLength = 128;
    private const int MaxDisplayNameLength = 200;
    private const int MaxNoteLength = 2000;
    private const int MaxDeviceInstallIds = 64;

    public static void ValidateAndThrow(CompiledPolicy policy)
    {
        IReadOnlyList<PolicyValidationError> errors = Validate(policy);
        if (errors.Count != 0)
        {
            throw new PolicyValidationException(errors);
        }
    }

    public static IReadOnlyList<PolicyValidationError> Validate(CompiledPolicy policy)
    {
        ArgumentNullException.ThrowIfNull(policy);
        List<PolicyValidationError> errors = [];

        if (policy.SchemaVersion != 1)
        {
            errors.Add(new("policy.schema_version", "Only schema version 1 is supported."));
        }

        if (policy.PolicyVersion < 0)
        {
            errors.Add(new("policy.version", "Policy version cannot be negative."));
        }

        if (policy.WorkstationId == Guid.Empty || policy.GroupId == Guid.Empty)
        {
            errors.Add(new("policy.scope_ids", "Workstation and group IDs are required."));
        }

        if (policy.GeneratedAtUtc.Offset != TimeSpan.Zero)
        {
            errors.Add(new("policy.generated_at", "GeneratedAtUtc must use the UTC offset."));
        }

        HashSet<Guid> entryIds = [];
        foreach (DeviceEntry entry in policy.Entries)
        {
            ValidateEntry(policy, entry, entryIds, errors);
        }

        return errors;
    }

    private static void ValidateEntry(
        CompiledPolicy policy,
        DeviceEntry entry,
        HashSet<Guid> entryIds,
        List<PolicyValidationError> errors)
    {
        if (entry.Id == Guid.Empty || !entryIds.Add(entry.Id))
        {
            errors.Add(new("entry.id", "Entry ID must be non-empty and unique.", entry.Id));
        }

        bool fieldsValid = entry.MatchType switch
        {
            MatchType.InstanceId =>
                HasText(entry.InstanceId) &&
                entry.Vid is null &&
                entry.Pid is null &&
                entry.Serial is null &&
                entry.DeviceClass is null,
            MatchType.VidPidSerial =>
                IsHexId(entry.Vid) &&
                IsHexId(entry.Pid) &&
                HasText(entry.Serial) &&
                entry.InstanceId is null &&
                entry.DeviceClass is null,
            MatchType.VidPid =>
                IsHexId(entry.Vid) &&
                IsHexId(entry.Pid) &&
                entry.Serial is null &&
                entry.InstanceId is null &&
                entry.DeviceClass is null,
            MatchType.DeviceClass =>
                entry.DeviceClass is not null &&
                entry.Vid is null &&
                entry.Pid is null &&
                entry.Serial is null &&
                entry.InstanceId is null,
            _ => false,
        };

        if (!fieldsValid)
        {
            errors.Add(new(
                "entry.match_fields",
                "Exactly the fields required by MatchType must be supplied.",
                entry.Id));
        }

        if (entry.InstanceId?.Length > MaxInstanceIdLength ||
            entry.Serial?.Length > MaxSerialLength ||
            entry.DisplayName.Length is 0 or > MaxDisplayNameLength ||
            entry.Note?.Length > MaxNoteLength)
        {
            errors.Add(new("entry.string_length", "One or more entry strings exceed their limits.", entry.Id));
        }

        if (entry.Mandatory &&
            (entry.ScopeType != ScopeType.Global || entry.Action != EntryAction.Block))
        {
            errors.Add(new(
                "entry.mandatory",
                "Mandatory is valid only for global block entries.",
                entry.Id));
        }

        bool scopeValid = entry.ScopeType switch
        {
            ScopeType.Global => entry.ScopeId is null,
            ScopeType.Group => entry.ScopeId == policy.GroupId,
            ScopeType.Workstation => entry.ScopeId == policy.WorkstationId,
            _ => false,
        };

        if (!scopeValid)
        {
            errors.Add(new("entry.scope", "Entry scope does not match the compiled policy.", entry.Id));
        }

        if (entry.ExpiresAtUtc is not null &&
            entry.ExpiresAtUtc.Value.Offset != TimeSpan.Zero)
        {
            errors.Add(new("entry.expires_at", "ExpiresAtUtc must use the UTC offset.", entry.Id));
        }

        if (entry.DeviceInstallIds.Count > MaxDeviceInstallIds ||
            entry.DeviceInstallIds.Any(id => id.Length is 0 or > MaxInstanceIdLength) ||
            entry.DeviceInstallIds.Distinct(StringComparer.OrdinalIgnoreCase).Count() !=
            entry.DeviceInstallIds.Count)
        {
            errors.Add(new(
                "entry.device_install_ids",
                "Device installation IDs must be unique and within their limits.",
                entry.Id));
        }
    }

    private static bool HasText(string? value) => !string.IsNullOrWhiteSpace(value);

    private static bool IsHexId(string? value) =>
        value is not null && FourHexCharacters().IsMatch(value);

    [GeneratedRegex("^[0-9A-Fa-f]{4}$", RegexOptions.CultureInvariant)]
    private static partial Regex FourHexCharacters();
}
