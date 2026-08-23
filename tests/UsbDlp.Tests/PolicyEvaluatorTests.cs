using UsbDlp.Policy;
using MatchType = UsbDlp.Policy.MatchType;

namespace UsbDlp.Tests;

public sealed class PolicyEvaluatorTests
{
    [Fact]
    [Trait("TestId", "T-M1-01")]
    public void SafetyAllowListWinsOverMatchingBlock()
    {
        DeviceInfo keyboard = PolicyTestData.Device(
            classGuid: HardCodedSafetyAllowList.KeyboardClassGuid);
        DeviceEntry block = PolicyTestData.Entry(
            MatchType.DeviceClass,
            EntryAction.Block,
            deviceClass: ProductDeviceClass.UsbStorage);

        PolicyEvaluation result = PolicyEvaluator.Evaluate(
            keyboard,
            PolicyTestData.Policy(GroupMode.DefaultDeny, block),
            PolicyTestData.Now);

        Assert.Equal(Decision.Allow, result.Decision);
        Assert.Equal(PolicyReasonCodes.SafetyAllowList, result.ReasonCode);
        Assert.Null(result.MatchedEntryId);
    }

    [Fact]
    [Trait("TestId", "T-M1-02")]
    public void ExactInstanceAllowBeatsVidPidBlockWithinScope()
    {
        DeviceInfo device = PolicyTestData.Device();
        DeviceEntry broadBlock = PolicyTestData.Entry(
            MatchType.VidPid,
            EntryAction.Block,
            vid: "0781",
            pid: "5583");
        DeviceEntry exactAllow = PolicyTestData.Entry(
            MatchType.InstanceId,
            EntryAction.Allow,
            instanceId: device.InstanceId);

        PolicyEvaluation result = PolicyEvaluator.Evaluate(
            device,
            PolicyTestData.Policy(GroupMode.DefaultDeny, broadBlock, exactAllow),
            PolicyTestData.Now);

        Assert.Equal(Decision.Allow, result.Decision);
        Assert.Equal(exactAllow.Id, result.MatchedEntryId);
    }

    [Fact]
    [Trait("TestId", "T-M1-03")]
    public void BlockBeatsAllowWithinSameScopeAndSpecificity()
    {
        DeviceInfo device = PolicyTestData.Device();
        DeviceEntry allow = PolicyTestData.Entry(
            MatchType.VidPid,
            EntryAction.Allow,
            vid: "0781",
            pid: "5583");
        DeviceEntry block = PolicyTestData.Entry(
            MatchType.VidPid,
            EntryAction.Block,
            vid: "0781",
            pid: "5583");

        PolicyEvaluation result = PolicyEvaluator.Evaluate(
            device,
            PolicyTestData.Policy(GroupMode.DefaultAllow, allow, block),
            PolicyTestData.Now);

        Assert.Equal(Decision.Block, result.Decision);
        Assert.Equal(block.Id, result.MatchedEntryId);
    }

    [Fact]
    [Trait("TestId", "T-M1-04")]
    public void ExpiredEntryIsIgnored()
    {
        DeviceEntry expiredBlock = PolicyTestData.Entry(
            MatchType.VidPid,
            EntryAction.Block,
            vid: "0781",
            pid: "5583",
            expiresAtUtc: PolicyTestData.Now.AddTicks(-1));

        PolicyEvaluation result = PolicyEvaluator.Evaluate(
            PolicyTestData.Device(),
            PolicyTestData.Policy(GroupMode.DefaultAllow, expiredBlock),
            PolicyTestData.Now);

        Assert.Equal(Decision.Allow, result.Decision);
        Assert.Equal(PolicyReasonCodes.DefaultAllow, result.ReasonCode);
    }

    [Fact]
    [Trait("TestId", "T-M1-05")]
    public void VidPidMatchesAnySerial()
    {
        DeviceEntry wildcardBlock = PolicyTestData.Entry(
            MatchType.VidPid,
            EntryAction.Block,
            vid: "0781",
            pid: "5583");

        PolicyEvaluation first = PolicyEvaluator.Evaluate(
            PolicyTestData.Device(serial: "SERIAL-A"),
            PolicyTestData.Policy(GroupMode.DefaultAllow, wildcardBlock),
            PolicyTestData.Now);
        PolicyEvaluation second = PolicyEvaluator.Evaluate(
            PolicyTestData.Device(serial: null),
            PolicyTestData.Policy(GroupMode.DefaultAllow, wildcardBlock),
            PolicyTestData.Now);

        Assert.Equal(Decision.Block, first.Decision);
        Assert.Equal(Decision.Block, second.Decision);
    }

    [Fact]
    [Trait("TestId", "T-M1-06")]
    public void NoMatchInDefaultDenyBlocks()
    {
        PolicyEvaluation result = PolicyEvaluator.Evaluate(
            PolicyTestData.Device(),
            PolicyTestData.Policy(GroupMode.DefaultDeny),
            PolicyTestData.Now);

        Assert.Equal(Decision.Block, result.Decision);
        Assert.Equal(PolicyReasonCodes.DefaultDeny, result.ReasonCode);
    }

    [Fact]
    [Trait("TestId", "T-M1-07")]
    public void NoMatchInDefaultAllowAllows()
    {
        PolicyEvaluation result = PolicyEvaluator.Evaluate(
            PolicyTestData.Device(),
            PolicyTestData.Policy(GroupMode.DefaultAllow),
            PolicyTestData.Now);

        Assert.Equal(Decision.Allow, result.Decision);
        Assert.Equal(PolicyReasonCodes.DefaultAllow, result.ReasonCode);
    }

    [Fact]
    [Trait("TestId", "T-M1-08")]
    public void VidPidComparisonIsCaseInsensitive()
    {
        DeviceEntry block = PolicyTestData.Entry(
            MatchType.VidPid,
            EntryAction.Block,
            vid: "aBcD",
            pid: "eF01");

        PolicyEvaluation result = PolicyEvaluator.Evaluate(
            PolicyTestData.Device(vid: "ABCD", pid: "EF01"),
            PolicyTestData.Policy(GroupMode.DefaultAllow, block),
            PolicyTestData.Now);

        Assert.Equal(Decision.Block, result.Decision);
    }

    [Theory]
    [InlineData("FFFF", "5583")]
    [InlineData("0781", "FFFF")]
    public void VidPidRequiresBothIdentifiersToMatch(string vid, string pid)
    {
        DeviceEntry allow = PolicyTestData.Entry(
            MatchType.VidPid,
            EntryAction.Allow,
            vid: "0781",
            pid: "5583");

        PolicyEvaluation result = PolicyEvaluator.Evaluate(
            PolicyTestData.Device(vid: vid, pid: pid),
            PolicyTestData.Policy(GroupMode.DefaultDeny, allow),
            PolicyTestData.Now);

        Assert.Equal(Decision.Block, result.Decision);
    }

    [Fact]
    [Trait("TestId", "T-M1-09")]
    public void ExpiryBoundaryIsExpired()
    {
        DeviceEntry expiringBlock = PolicyTestData.Entry(
            MatchType.VidPid,
            EntryAction.Block,
            vid: "0781",
            pid: "5583",
            expiresAtUtc: PolicyTestData.Now);

        PolicyEvaluation result = PolicyEvaluator.Evaluate(
            PolicyTestData.Device(),
            PolicyTestData.Policy(GroupMode.DefaultAllow, expiringBlock),
            PolicyTestData.Now);

        Assert.Equal(Decision.Allow, result.Decision);
    }

    [Fact]
    [Trait("TestId", "T-M1-10")]
    public void JsonEntryOrderDoesNotAffectEvaluation()
    {
        DeviceEntry allow = PolicyTestData.Entry(
            MatchType.VidPid,
            EntryAction.Allow,
            vid: "0781",
            pid: "5583");
        DeviceEntry block = PolicyTestData.Entry(
            MatchType.VidPid,
            EntryAction.Block,
            vid: "0781",
            pid: "5583");
        CompiledPolicy firstPolicy = PolicyTestData.Policy(GroupMode.DefaultAllow, allow, block);
        CompiledPolicy secondPolicy = firstPolicy with { Entries = [block, allow] };

        PolicyEvaluation first = PolicyEvaluator.Evaluate(
            PolicyTestData.Device(), firstPolicy, PolicyTestData.Now);
        PolicyEvaluation second = PolicyEvaluator.Evaluate(
            PolicyTestData.Device(), secondPolicy, PolicyTestData.Now);

        Assert.Equal(first, second);
    }

    [Fact]
    [Trait("TestId", "T-M1-11")]
    public void MalformedMatchFieldCombinationIsRejected()
    {
        DeviceEntry malformed = PolicyTestData.Entry(
            MatchType.VidPid,
            EntryAction.Allow,
            vid: "0781",
            pid: null,
            instanceId: @"USB\SHOULD_NOT_BE_PRESENT");

        PolicyValidationException exception = Assert.Throws<PolicyValidationException>(
            () => PolicyEvaluator.Evaluate(
                PolicyTestData.Device(),
                PolicyTestData.Policy(GroupMode.DefaultDeny, malformed),
                PolicyTestData.Now));

        Assert.Contains(exception.Errors, error => error.Code == "entry.match_fields");
    }

    [Fact]
    [Trait("TestId", "T-M1-12")]
    public void MandatoryGlobalThenWorkstationGroupGlobalScopePrecedenceIsApplied()
    {
        DeviceInfo device = PolicyTestData.Device();
        DeviceEntry globalExactBlock = PolicyTestData.Entry(
            MatchType.InstanceId,
            EntryAction.Block,
            instanceId: device.InstanceId);
        DeviceEntry groupClassBlock = PolicyTestData.Entry(
            MatchType.DeviceClass,
            EntryAction.Block,
            ScopeType.Group,
            PolicyTestData.GroupId,
            deviceClass: ProductDeviceClass.UsbStorage);
        DeviceEntry workstationClassAllow = PolicyTestData.Entry(
            MatchType.DeviceClass,
            EntryAction.Allow,
            ScopeType.Workstation,
            PolicyTestData.WorkstationId,
            deviceClass: ProductDeviceClass.UsbStorage);

        PolicyEvaluation workstationWins = PolicyEvaluator.Evaluate(
            device,
            PolicyTestData.Policy(
                GroupMode.DefaultDeny,
                globalExactBlock,
                groupClassBlock,
                workstationClassAllow),
            PolicyTestData.Now);

        DeviceEntry mandatoryGlobalBlock = globalExactBlock with { Mandatory = true };
        PolicyEvaluation mandatoryWins = PolicyEvaluator.Evaluate(
            device,
            PolicyTestData.Policy(
                GroupMode.DefaultDeny,
                mandatoryGlobalBlock,
                workstationClassAllow),
            PolicyTestData.Now);

        Assert.Equal(Decision.Allow, workstationWins.Decision);
        Assert.Equal(workstationClassAllow.Id, workstationWins.MatchedEntryId);
        Assert.Equal(Decision.Block, mandatoryWins.Decision);
        Assert.Equal(mandatoryGlobalBlock.Id, mandatoryWins.MatchedEntryId);
        Assert.Equal(PolicyReasonCodes.MandatoryGlobalBlock, mandatoryWins.ReasonCode);
    }

    [Fact]
    [Trait("TestId", "T-M1-13")]
    public void EvaluationIncludesDecisionMetadata()
    {
        Guid entryId = Guid.Parse("00000000-0000-0000-0000-000000000001");
        DeviceEntry block = PolicyTestData.Entry(
            MatchType.VidPid,
            EntryAction.Block,
            vid: "0781",
            pid: "5583",
            id: entryId);

        PolicyEvaluation result = PolicyEvaluator.Evaluate(
            PolicyTestData.Device(),
            PolicyTestData.Policy(GroupMode.DefaultAllow, block),
            PolicyTestData.Now);

        Assert.Equal(Decision.Block, result.Decision);
        Assert.Equal(entryId, result.MatchedEntryId);
        Assert.Equal(PolicyReasonCodes.EntryBlock, result.ReasonCode);
        Assert.Equal(42, result.PolicyVersion);
        Assert.Equal(PolicyTestData.Now, result.EvaluatedAtUtc);
    }

    [Fact]
    public void SafetyAllowListChecksDeviceAncestry()
    {
        DeviceInfo compositeStorage = PolicyTestData.Device(
            ancestryClassGuids: [HardCodedSafetyAllowList.HidClassGuid]);

        PolicyEvaluation result = PolicyEvaluator.Evaluate(
            compositeStorage,
            PolicyTestData.Policy(GroupMode.DefaultDeny),
            PolicyTestData.Now);

        Assert.Equal(Decision.Allow, result.Decision);
        Assert.Equal(PolicyReasonCodes.SafetyAllowList, result.ReasonCode);
    }

    [Fact]
    public void GroupEntryBeatsMoreSpecificGlobalEntry()
    {
        DeviceInfo device = PolicyTestData.Device();
        DeviceEntry globalInstanceBlock = PolicyTestData.Entry(
            MatchType.InstanceId,
            EntryAction.Block,
            instanceId: device.InstanceId);
        DeviceEntry groupClassAllow = PolicyTestData.Entry(
            MatchType.DeviceClass,
            EntryAction.Allow,
            ScopeType.Group,
            PolicyTestData.GroupId,
            deviceClass: ProductDeviceClass.UsbStorage);

        PolicyEvaluation result = PolicyEvaluator.Evaluate(
            device,
            PolicyTestData.Policy(GroupMode.DefaultDeny, globalInstanceBlock, groupClassAllow),
            PolicyTestData.Now);

        Assert.Equal(Decision.Allow, result.Decision);
        Assert.Equal(groupClassAllow.Id, result.MatchedEntryId);
    }

    [Fact]
    public void GlobalEntryIsSelectedWhenHigherScopesDoNotMatch()
    {
        DeviceEntry globalAllow = PolicyTestData.Entry(
            MatchType.DeviceClass,
            EntryAction.Allow,
            deviceClass: ProductDeviceClass.UsbStorage);

        PolicyEvaluation result = PolicyEvaluator.Evaluate(
            PolicyTestData.Device(),
            PolicyTestData.Policy(GroupMode.DefaultDeny, globalAllow),
            PolicyTestData.Now);

        Assert.Equal(Decision.Allow, result.Decision);
        Assert.Equal(globalAllow.Id, result.MatchedEntryId);
    }

    [Fact]
    public void VidPidSerialRequiresEveryIdentifierToMatch()
    {
        DeviceEntry exactAllow = PolicyTestData.Entry(
            MatchType.VidPidSerial,
            EntryAction.Allow,
            vid: "0781",
            pid: "5583",
            serial: "4C530001230904");
        CompiledPolicy policy = PolicyTestData.Policy(GroupMode.DefaultDeny, exactAllow);

        Assert.Equal(
            Decision.Allow,
            PolicyEvaluator.Evaluate(PolicyTestData.Device(), policy, PolicyTestData.Now).Decision);
        Assert.Equal(
            Decision.Block,
            PolicyEvaluator.Evaluate(
                PolicyTestData.Device(vid: "FFFF"), policy, PolicyTestData.Now).Decision);
        Assert.Equal(
            Decision.Block,
            PolicyEvaluator.Evaluate(
                PolicyTestData.Device(pid: "FFFF"), policy, PolicyTestData.Now).Decision);
        Assert.Equal(
            Decision.Block,
            PolicyEvaluator.Evaluate(
                PolicyTestData.Device(serial: "OTHER"), policy, PolicyTestData.Now).Decision);
    }

    [Fact]
    public void InstanceAndClassNonMatchesAreIgnored()
    {
        DeviceEntry otherInstance = PolicyTestData.Entry(
            MatchType.InstanceId,
            EntryAction.Allow,
            instanceId: @"USB\OTHER");
        DeviceEntry otherClass = PolicyTestData.Entry(
            MatchType.DeviceClass,
            EntryAction.Allow,
            deviceClass: ProductDeviceClass.Wpd);

        PolicyEvaluation result = PolicyEvaluator.Evaluate(
            PolicyTestData.Device(),
            PolicyTestData.Policy(GroupMode.DefaultDeny, otherInstance, otherClass),
            PolicyTestData.Now);

        Assert.Equal(Decision.Block, result.Decision);
        Assert.Equal(PolicyReasonCodes.DefaultDeny, result.ReasonCode);
    }

    [Fact]
    public void ActiveFutureExpiryIsApplied()
    {
        DeviceEntry futureBlock = PolicyTestData.Entry(
            MatchType.VidPid,
            EntryAction.Block,
            vid: "0781",
            pid: "5583",
            expiresAtUtc: PolicyTestData.Now.AddMinutes(1));

        PolicyEvaluation result = PolicyEvaluator.Evaluate(
            PolicyTestData.Device(),
            PolicyTestData.Policy(GroupMode.DefaultAllow, futureBlock),
            PolicyTestData.Now);

        Assert.Equal(Decision.Block, result.Decision);
    }

    [Fact]
    public void EvaluationRejectsNonUtcClock()
    {
        DateTimeOffset localClock = PolicyTestData.Now.ToOffset(TimeSpan.FromHours(1));

        ArgumentException exception = Assert.Throws<ArgumentException>(
            () => PolicyEvaluator.Evaluate(
                PolicyTestData.Device(),
                PolicyTestData.Policy(GroupMode.DefaultAllow),
                localClock));

        Assert.Equal("nowUtc", exception.ParamName);
    }
}
