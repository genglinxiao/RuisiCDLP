using System.IO.Pipes;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;
using UsbDlp.Contracts;
using UsbDlp.Notifications;
using UsbDlp.Policy;

namespace UsbDlp.Agent;

public sealed partial class NotifierPipeServer(AgentStorage storage, AgentApiClient apiClient, InteractiveNotifierLauncher launcher, ILogger<NotifierPipeServer> logger) : BackgroundService
{
    public const string PipeName = "UsbDlp.Agent.Notifier.v1";
    private readonly Channel<NotifierMessage> _messages = Channel.CreateBounded<NotifierMessage>(new BoundedChannelOptions(100) { FullMode = BoundedChannelFullMode.DropOldest });
    private readonly Dictionary<string, DateTimeOffset> _requestCooldowns = new(StringComparer.OrdinalIgnoreCase);
    private readonly object _cooldownSync = new();
    private readonly ILogger<NotifierPipeServer> _logger = logger;
    private NotifierMessage? _latestBlocked;
    private int _connectedClients;

    public bool Publish(NotifierMessage message)
    {
        if (message.Type == "blocked" && message.Device is not null)
        {
            lock (_cooldownSync)
            {
                if (_requestCooldowns.TryGetValue(message.Device.InstanceId, out DateTimeOffset retryAfter))
                {
                    if (retryAfter > DateTimeOffset.UtcNow)
                        message = message with { Status = "recent", RetryAfterUtc = retryAfter };
                    else
                        _requestCooldowns.Remove(message.Device.InstanceId);
                }
            }
            Volatile.Write(ref _latestBlocked, message);
            if (Volatile.Read(ref _connectedClients) == 0)
                launcher.EnsureStarted();
        }
        else if (message.Type == "allowed" && message.Device is not null)
        {
            NotifierMessage? latest = Volatile.Read(ref _latestBlocked);
            if (string.Equals(latest?.Device?.InstanceId, message.Device.InstanceId, StringComparison.OrdinalIgnoreCase))
                Interlocked.CompareExchange(ref _latestBlocked, null, latest);
        }
        return _messages.Writer.TryWrite(message);
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        while (!stoppingToken.IsCancellationRequested)
        {
            try
            {
                await using NamedPipeServerStream pipe = CreatePipe();
                await pipe.WaitForConnectionAsync(stoppingToken);
                Interlocked.Increment(ref _connectedClients);
                LogConnected();
                try
                {
                    using StreamReader reader = new(pipe, Encoding.UTF8, false, 4096, leaveOpen: true);
                    using StreamWriter writer = new(pipe, new UTF8Encoding(false), 4096, leaveOpen: true) { AutoFlush = true };
                    using CancellationTokenSource connection = CancellationTokenSource.CreateLinkedTokenSource(stoppingToken);
                    Task send = SendAsync(writer, connection.Token);
                    Task receive = ReceiveAsync(reader, connection.Token);
                    await Task.WhenAny(send, receive);
                    await connection.CancelAsync();
                    try
                    {
                        await Task.WhenAll(send, receive);
                    }
                    catch (OperationCanceledException) when (connection.IsCancellationRequested) { }
                    catch (IOException exception) { LogPipeError(exception.Message); }
                }
                finally { Interlocked.Decrement(ref _connectedClients); }
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested) { }
            catch (IOException exception) { LogPipeError(exception.Message); }
        }
    }

    private static NamedPipeServerStream CreatePipe()
    {
        PipeSecurity security = new();
        security.AddAccessRule(new PipeAccessRule(
            new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null),
            PipeAccessRights.FullControl,
            AccessControlType.Allow));
        security.AddAccessRule(new PipeAccessRule(
            new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null),
            PipeAccessRights.FullControl,
            AccessControlType.Allow));
        security.AddAccessRule(new PipeAccessRule(
            new SecurityIdentifier(WellKnownSidType.AuthenticatedUserSid, null),
            PipeAccessRights.ReadWrite | PipeAccessRights.Synchronize,
            AccessControlType.Allow));
        return NamedPipeServerStreamAcl.Create(
            PipeName,
            PipeDirection.InOut,
            10,
            // JSON is newline-delimited; byte mode preserves stream framing across the
            // multiple native writes StreamWriter may use for one serialized message.
            PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous,
            64 * 1024,
            64 * 1024,
            security);
    }

    private async Task SendAsync(StreamWriter writer, CancellationToken cancellationToken)
    {
        // A notifier often starts after initial device evaluation. Replay the current block so
        // reconnects always have enough context to render and submit an access request.
        NotifierMessage? latest = Volatile.Read(ref _latestBlocked);
        if (latest is not null)
            await writer.WriteLineAsync(JsonSerializer.Serialize(latest, NotifierJson.Options));
        await foreach (NotifierMessage message in _messages.Reader.ReadAllAsync(cancellationToken))
            await writer.WriteLineAsync(JsonSerializer.Serialize(message, NotifierJson.Options));
    }

    private async Task ReceiveAsync(StreamReader reader, CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            string? line = await reader.ReadLineAsync(cancellationToken);
            if (line is null || line.Length > 64 * 1024) return;
            NotifierAccessRequest? request = JsonSerializer.Deserialize<NotifierAccessRequest>(line, NotifierJson.Options);
            if (request?.Type != "request_access") continue;
            NotifierMessage response;
            try
            {
                AgentIdentity? identity = await storage.LoadIdentityAsync(cancellationToken);
                if (identity is null) throw new InvalidOperationException("Agent is not enrolled.");
                AccessRequestCreate create = new(
                    1, request.EventId, DateTimeOffset.UtcNow,
                    WindowsIdentity.GetCurrent().User?.Value ?? "S-1-0-0",
                    Environment.UserName, request.Device, request.RequestedDurationDays, request.Justification);
                AccessRequestResponse result = await apiClient.RequestAccessAsync(identity, create, cancellationToken);
                if (result.RetryAfterUtc is DateTimeOffset retryAfter)
                {
                    lock (_cooldownSync)
                        _requestCooldowns[request.Device.InstanceId] = retryAfter;
                }
                response = new(1, "request_result", request.EventId, request.Device, null, result.RequestId, result.Status, null, result.RetryAfterUtc);
            }
            catch (Exception exception)
            {
                response = new(1, "request_result", request.EventId, request.Device, null, null, "error", exception.Message);
            }
            _messages.Writer.TryWrite(response);
        }
    }

    [LoggerMessage(EventId=1200,Level=LogLevel.Information,Message="Notifier connected")]
    private partial void LogConnected();
    [LoggerMessage(EventId=1299,Level=LogLevel.Warning,Message="Notifier pipe error: {Error}")]
    private partial void LogPipeError(string error);
}
