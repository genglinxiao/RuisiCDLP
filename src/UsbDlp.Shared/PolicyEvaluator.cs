namespace UsbDlp.Policy;

public static class PolicyReasonCodes
{
    public const string SafetyAllowList = "safety_allowlist";
    public const string MandatoryGlobalBlock = "mandatory_global_block";
    public const string EntryAllow = "entry_allow";
    public const string EntryBlock = "entry_block";
    public const string DefaultAllow = "default_allow";
    public const string DefaultDeny = "default_deny";
}

public static class PolicyEvaluator
{
    private static readonly ScopeType[] ScopePrecedence =
    [
        ScopeType.Workstation,
        ScopeType.Group,
        ScopeType.Global,
    ];

    private static readonly Dictionary<MatchType, int> SpecificityByMatchType =
        new Dictionary<MatchType, int>
        {
            [MatchType.InstanceId] = 4,
            [MatchType.VidPidSerial] = 3,
            [MatchType.VidPid] = 2,
            [MatchType.DeviceClass] = 1,
        };

    public static PolicyEvaluation Evaluate(
        DeviceInfo device,
        CompiledPolicy policy,
        DateTimeOffset nowUtc)
    {
        ArgumentNullException.ThrowIfNull(device);
        ArgumentNullException.ThrowIfNull(policy);

        if (nowUtc.Offset != TimeSpan.Zero)
        {
            throw new ArgumentException("Evaluation time must use the UTC offset.", nameof(nowUtc));
        }

        CompiledPolicyValidator.ValidateAndThrow(policy);

        if (HardCodedSafetyAllowList.Contains(device.ClassGuid) ||
            HardCodedSafetyAllowList.ContainsAny(device.AncestryClassGuids))
        {
            return Result(Decision.Allow, null, PolicyReasonCodes.SafetyAllowList, policy, nowUtc);
        }

        DeviceEntry[] matches = policy.Entries
            .Where(entry => IsActive(entry, nowUtc))
            .Where(entry => Matches(device, entry))
            .ToArray();

        DeviceEntry? mandatoryBlock = SelectEntry(
            matches.Where(entry => entry.Mandatory));
        if (mandatoryBlock is not null)
        {
            return Result(
                Decision.Block,
                mandatoryBlock.Id,
                PolicyReasonCodes.MandatoryGlobalBlock,
                policy,
                nowUtc);
        }

        foreach (ScopeType scope in ScopePrecedence)
        {
            DeviceEntry? selected = SelectEntry(
                matches.Where(entry => entry.ScopeType == scope));
            if (selected is not null)
            {
                Decision decision = selected.Action == EntryAction.Block
                    ? Decision.Block
                    : Decision.Allow;
                string reason = decision == Decision.Block
                    ? PolicyReasonCodes.EntryBlock
                    : PolicyReasonCodes.EntryAllow;
                return Result(decision, selected.Id, reason, policy, nowUtc);
            }
        }

        return policy.GroupMode == GroupMode.DefaultDeny
            ? Result(Decision.Block, null, PolicyReasonCodes.DefaultDeny, policy, nowUtc)
            : Result(Decision.Allow, null, PolicyReasonCodes.DefaultAllow, policy, nowUtc);
    }

    private static DeviceEntry? SelectEntry(IEnumerable<DeviceEntry> entries) =>
        entries
            .OrderByDescending(entry => Specificity(entry.MatchType))
            .ThenByDescending(entry => entry.Action == EntryAction.Block)
            .ThenBy(entry => entry.Id)
            .FirstOrDefault();

    private static bool Matches(DeviceInfo device, DeviceEntry entry) =>
        entry.MatchType switch
        {
            MatchType.InstanceId =>
                EqualsIgnoreCase(device.InstanceId, entry.InstanceId),
            MatchType.VidPidSerial =>
                EqualsIgnoreCase(device.Vid, entry.Vid) &&
                EqualsIgnoreCase(device.Pid, entry.Pid) &&
                EqualsIgnoreCase(device.Serial, entry.Serial),
            MatchType.VidPid =>
                EqualsIgnoreCase(device.Vid, entry.Vid) &&
                EqualsIgnoreCase(device.Pid, entry.Pid),
            MatchType.DeviceClass =>
                device.DeviceClass == entry.DeviceClass,
            _ => false,
        };

    private static bool IsActive(DeviceEntry entry, DateTimeOffset nowUtc)
    {
        if (!entry.ExpiresAtUtc.HasValue)
        {
            return true;
        }

        return entry.ExpiresAtUtc.Value > nowUtc;
    }

    private static int Specificity(MatchType matchType) =>
        SpecificityByMatchType[matchType];

    private static bool EqualsIgnoreCase(string? left, string? right) =>
        string.Equals(left, right, StringComparison.OrdinalIgnoreCase);

    private static PolicyEvaluation Result(
        Decision decision,
        Guid? matchedEntryId,
        string reasonCode,
        CompiledPolicy policy,
        DateTimeOffset evaluatedAtUtc) =>
        new(decision, matchedEntryId, reasonCode, policy.PolicyVersion, evaluatedAtUtc);
}
