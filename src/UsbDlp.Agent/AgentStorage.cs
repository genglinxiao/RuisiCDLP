using System.Security.Cryptography;
using System.Text.Json;
using UsbDlp.Policy;

namespace UsbDlp.Agent;

public sealed record AgentIdentity(Guid WorkstationId, string ApiCredential, Guid ClientNonce);

public sealed class AgentStorage
{
    private readonly string _directory;

    public AgentStorage(AgentOptions options)
    {
        _directory = Path.GetFullPath(options.DataDirectory);
        Directory.CreateDirectory(_directory);
    }

    public string EventDatabasePath => Path.Combine(_directory, "events.db");

    public async Task<AgentIdentity?> LoadIdentityAsync(CancellationToken cancellationToken) =>
        await LoadProtectedAsync<AgentIdentity>("secrets.dat", cancellationToken);

    public Task SaveIdentityAsync(AgentIdentity identity, CancellationToken cancellationToken) =>
        SaveProtectedAsync("secrets.dat", identity, cancellationToken);

    public async Task<CompiledPolicy?> LoadPolicyAsync(CancellationToken cancellationToken) =>
        await LoadProtectedAsync<CompiledPolicy>("policy.dat", cancellationToken);

    public Task SavePolicyAsync(CompiledPolicy policy, CancellationToken cancellationToken) =>
        SaveProtectedAsync("policy.dat", policy, cancellationToken);

    private async Task<T?> LoadProtectedAsync<T>(string name, CancellationToken cancellationToken)
    {
        string path = Path.Combine(_directory, name);
        if (!File.Exists(path)) return default;
        byte[] protectedBytes = await File.ReadAllBytesAsync(path, cancellationToken);
        byte[] plain = ProtectedData.Unprotect(protectedBytes, null, DataProtectionScope.LocalMachine);
        return JsonSerializer.Deserialize<T>(plain, PolicyJson.Options);
    }

    private async Task SaveProtectedAsync<T>(string name, T value, CancellationToken cancellationToken)
    {
        byte[] plain = JsonSerializer.SerializeToUtf8Bytes(value, PolicyJson.Options);
        byte[] protectedBytes = ProtectedData.Protect(plain, null, DataProtectionScope.LocalMachine);
        string path = Path.Combine(_directory, name);
        string temporary = path + ".tmp";
        await File.WriteAllBytesAsync(temporary, protectedBytes, cancellationToken);
        File.Move(temporary, path, true);
    }
}
