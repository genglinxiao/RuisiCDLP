using System.Text.Json;
using UsbDlp.Contracts;
using UsbDlp.Policy;
using UsbDlp.Windows;

namespace UsbDlp.Agent;

public sealed partial class Worker(
    AgentOptions options,
    AgentStorage storage,
    DurableEventStore eventQueue,
    AgentApiClient apiClient,
    IWindowsDeviceEnumerator enumerator,
    IWindowsDeviceEnforcer enforcer,
    IRegistryPolicyManager registryPolicy,
    NotifierPipeServer notifier,
    ILogger<Worker> logger) : BackgroundService
{
    private readonly ILogger<Worker> _logger = logger;
    private readonly Dictionary<string, (Decision Decision, long Version)> _lastDecisions = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, DeviceInfo> _knownDevices = new(StringComparer.OrdinalIgnoreCase);
    private readonly HashSet<string> _presentDeviceIds = new(StringComparer.OrdinalIgnoreCase);

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        await eventQueue.InitializeAsync(stoppingToken);
        AgentIdentity? identity = await storage.LoadIdentityAsync(stoppingToken);
        CompiledPolicy? policy = await storage.LoadPolicyAsync(stoppingToken);
        Guid nonce = identity?.ClientNonce ?? Guid.NewGuid();

        while (!stoppingToken.IsCancellationRequested)
        {
            try
            {
                identity ??= await apiClient.EnrollAsync(nonce, stoppingToken);
                await storage.SaveIdentityAsync(identity, stoppingToken);
                if (!string.IsNullOrWhiteSpace(options.EnrollmentTokenFile) && File.Exists(options.EnrollmentTokenFile))
                    File.Delete(options.EnrollmentTokenFile);

                CompiledPolicy? downloaded = await apiClient.FetchPolicyAsync(identity, policy?.PolicyVersion, stoppingToken);
                if (downloaded is not null)
                {
                    CompiledPolicyValidator.ValidateAndThrow(downloaded);
                    policy = downloaded;
                    await storage.SavePolicyAsync(policy, stoppingToken);
                    RegistryPolicyResult registryResult = registryPolicy.Apply(registryPolicy.BuildPlan(policy, DateTimeOffset.UtcNow), options.DryRun);
                    if (registryResult.Conflicts.Count != 0)
                        LogRegistryConflict(string.Join(", ", registryResult.Conflicts));
                    await EnqueueLifecycleAsync(identity, "policy_applied", policy.PolicyVersion, stoppingToken);
                    _lastDecisions.Clear();
                }

                policy ??= FailClosed(identity.WorkstationId);
                await EvaluatePresentDevicesAsync(identity, policy, stoppingToken);
                await UploadPendingAsync(identity, stoppingToken);
                int depth = await eventQueue.DepthAsync(stoppingToken);
                HeartbeatResponse heartbeat = await apiClient.HeartbeatAsync(identity, new(
                    1,
                    AgentApiClient.AgentVersion,
                    policy.PolicyVersion,
                    policy.PolicyVersion == 0 ? AgentHealth.FailClosed : AgentHealth.Healthy,
                    depth,
                    null,
                    DateTimeOffset.UtcNow), stoppingToken);
                LogHeartbeat(policy.PolicyVersion, depth, options.DryRun);
                if (heartbeat.PolicyChanged)
                {
                    CompiledPolicy? changed = await apiClient.FetchPolicyAsync(identity, null, stoppingToken);
                    if (changed is not null)
                    {
                        policy = changed;
                        await storage.SavePolicyAsync(policy, stoppingToken);
                        registryPolicy.Apply(registryPolicy.BuildPlan(policy, DateTimeOffset.UtcNow), options.DryRun);
                        _lastDecisions.Clear();
                    }
                }
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
                break;
            }
            catch (Exception exception)
            {
                LogSyncFailure(exception.GetType().Name, exception.Message);
                if (identity is not null)
                {
                    policy ??= FailClosed(identity.WorkstationId);
                    await EvaluatePresentDevicesAsync(identity, policy, stoppingToken);
                }
            }

            await Task.Delay(TimeSpan.FromSeconds(Math.Clamp(options.LocalPollSeconds, 2, 300)), stoppingToken);
        }
    }

    private async Task EvaluatePresentDevicesAsync(AgentIdentity identity, CompiledPolicy policy, CancellationToken cancellationToken)
    {
        IReadOnlyList<DeviceInfo> devices = enumerator.EnumerateInScopeDevices();
        HashSet<string> present = new(devices.Select(device => device.InstanceId), StringComparer.OrdinalIgnoreCase);
        HashSet<string> arrivals = new(present.Where(id => !_presentDeviceIds.Contains(id)), StringComparer.OrdinalIgnoreCase);
        foreach (string removed in _presentDeviceIds.Where(id => !present.Contains(id)))
            if (_knownDevices.TryGetValue(removed, out DeviceInfo? removedDevice))
                enforcer.Forget(removedDevice);
        _presentDeviceIds.Clear();
        _presentDeviceIds.UnionWith(present);

        foreach (DeviceInfo device in devices)
            _knownDevices[device.InstanceId] = device;
        foreach (string removed in _knownDevices.Keys.Where(id => !present.Contains(id) && !enforcer.Owns(_knownDevices[id])).ToArray())
        {
            _lastDecisions.Remove(removed);
            _knownDevices.Remove(removed);
        }

        IEnumerable<DeviceInfo> candidates = devices.Concat(
            _knownDevices.Values.Where(device => !present.Contains(device.InstanceId) && enforcer.Owns(device)));
        foreach (DeviceInfo device in candidates)
        {
            PolicyEvaluation evaluation = PolicyEvaluator.Evaluate(device, policy, DateTimeOffset.UtcNow);
            bool unchangedDecision = !arrivals.Contains(device.InstanceId) &&
                _lastDecisions.TryGetValue(device.InstanceId, out (Decision Decision, long Version) previous) &&
                previous == (evaluation.Decision, policy.PolicyVersion);
            if (unchangedDecision && (evaluation.Decision != Decision.Block || options.DryRun))
                continue;

            EnforcementResult enforcement = enforcer.Apply(device, evaluation.Decision, options.DryRun);
            if (unchangedDecision && enforcement.Operation == "already_disabled")
                continue;
            if (enforcement.ErrorCode is null)
                _lastDecisions[device.InstanceId] = (evaluation.Decision, policy.PolicyVersion);
            else
                _lastDecisions.Remove(device.InstanceId);
            LogDeviceDecision(device.InstanceId, evaluation.Decision, enforcement.Operation, enforcement.ErrorCode);
            AgentEvent item = new(
                Guid.NewGuid(),
                evaluation.Decision == Decision.Block ? "blocked" : "allowed",
                DateTimeOffset.UtcNow,
                Environment.MachineName,
                null,
                Environment.UserName,
                Snapshot(device),
                policy.PolicyVersion,
                evaluation.Decision,
                evaluation.MatchedEntryId,
                evaluation.ReasonCode,
                AgentApiClient.AgentVersion,
                JsonSerializer.SerializeToElement(new { enforcement.Operation, enforcement.DryRun, enforcement.ErrorCode }, PolicyJson.Options));
            await eventQueue.EnqueueAsync(item, cancellationToken);
            notifier.Publish(new(
                1,
                evaluation.Decision == Decision.Block ? "blocked" : "allowed",
                item.ClientEventId,
                item.Device,
                evaluation.ReasonCode,
                null,
                null,
                null));
        }
    }

    private async Task UploadPendingAsync(AgentIdentity identity, CancellationToken cancellationToken)
    {
        IReadOnlyList<AgentEvent> pending = await eventQueue.PendingAsync(50, cancellationToken);
        if (pending.Count == 0) return;
        EventBatchResponse response = await apiClient.UploadEventsAsync(identity, pending, cancellationToken);
        await eventQueue.MarkUploadedAsync(response.AcceptedClientEventIds.Concat(response.DuplicateClientEventIds), cancellationToken);
    }

    private Task EnqueueLifecycleAsync(AgentIdentity identity, string type, long policyVersion, CancellationToken cancellationToken) =>
        eventQueue.EnqueueAsync(new(
            Guid.NewGuid(), type, DateTimeOffset.UtcNow, Environment.MachineName, null, null, null,
            policyVersion, null, null, null, AgentApiClient.AgentVersion,
            JsonSerializer.SerializeToElement(new { identity.WorkstationId }, PolicyJson.Options)), cancellationToken);

    private static CompiledPolicy FailClosed(Guid workstationId) =>
        new(1, 0, DateTimeOffset.UtcNow, workstationId, Guid.Parse("00000000-0000-0000-0000-000000000001"), GroupMode.DefaultDeny, []);

    private static DeviceSnapshot Snapshot(DeviceInfo device) => new(
        device.InstanceId, device.HardwareIds, device.DeviceInstallIds, device.Vid, device.Pid,
        device.Serial, device.ClassGuid, device.Service, device.FriendlyName, device.Transport,
        device.Capabilities, device.Manufacturer, device.Model, device.Description);

    [LoggerMessage(EventId = 1100, Level = LogLevel.Information, Message = "Policy v{PolicyVersion}; queue {QueueDepth}; dry-run {DryRun}")]
    private partial void LogHeartbeat(long policyVersion, int queueDepth, bool dryRun);
    [LoggerMessage(EventId = 1101, Level = LogLevel.Information, Message = "Device {InstanceId}: {Decision}; {Operation}; error {ErrorCode}")]
    private partial void LogDeviceDecision(string instanceId, Decision decision, string operation, string? errorCode);
    [LoggerMessage(EventId = 1199, Level = LogLevel.Warning, Message = "Agent sync cycle failed ({ErrorType}): {ErrorMessage}")]
    private partial void LogSyncFailure(string errorType, string errorMessage);
    [LoggerMessage(EventId = 1102, Level = LogLevel.Warning, Message = "External device-install policy conflict: {Paths}")]
    private partial void LogRegistryConflict(string paths);
}
