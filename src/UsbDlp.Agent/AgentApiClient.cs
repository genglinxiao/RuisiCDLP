using System.Net;
using System.Net.Http.Headers;
using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using UsbDlp.Contracts;
using UsbDlp.Policy;

namespace UsbDlp.Agent;

public sealed class AgentApiClient : IDisposable
{
    private readonly AgentOptions _options;
    private readonly HttpClient _httpClient;

    public AgentApiClient(AgentOptions options)
    {
        _options = options;
        Uri baseUri = new(options.ServerBaseUrl.TrimEnd('/') + "/");
        if (baseUri.Scheme != Uri.UriSchemeHttps && (!options.AllowInsecureHttp || !baseUri.IsLoopback))
            throw new InvalidOperationException("HTTPS is required. Insecure HTTP may be enabled only for a loopback test server.");
        HttpClientHandler handler = new();
        handler.ServerCertificateCustomValidationCallback = ValidateCertificate;
        _httpClient = new(handler) { BaseAddress = baseUri, Timeout = TimeSpan.FromSeconds(20) };
    }

    public async Task<AgentIdentity> EnrollAsync(Guid nonce, CancellationToken cancellationToken)
    {
        string enrollmentToken = _options.EnrollmentToken;
        if (string.IsNullOrWhiteSpace(enrollmentToken) && !string.IsNullOrWhiteSpace(_options.EnrollmentTokenFile) && File.Exists(_options.EnrollmentTokenFile))
            enrollmentToken = (await File.ReadAllTextAsync(_options.EnrollmentTokenFile, cancellationToken)).Trim();
        if (string.IsNullOrWhiteSpace(enrollmentToken))
            throw new InvalidOperationException("UsbDlp:EnrollmentToken is required for first enrollment.");
        string machineSid = WindowsIdentity.GetCurrent().User?.Value ?? $"S-1-5-21-{Environment.MachineName.GetHashCode(StringComparison.Ordinal):X8}";
        EnrollmentRequest request = new(1, enrollmentToken, Environment.MachineName, machineSid, AgentVersion, nonce);
        using HttpResponseMessage response = await PostAsync("api/v1/agent/enroll", request, null, cancellationToken);
        response.EnsureSuccessStatusCode();
        EnrollmentResponse result = await ReadAsync<EnrollmentResponse>(response, cancellationToken);
        return new(result.WorkstationId, result.ApiCredential, nonce);
    }

    public async Task<CompiledPolicy?> FetchPolicyAsync(AgentIdentity identity, long? currentVersion, CancellationToken cancellationToken)
    {
        using HttpRequestMessage request = Authorized(HttpMethod.Get, "api/v1/agent/policy", identity);
        if (currentVersion.HasValue) request.Headers.IfNoneMatch.Add(new EntityTagHeaderValue($"\"{currentVersion}\""));
        using HttpResponseMessage response = await _httpClient.SendAsync(request, cancellationToken);
        if (response.StatusCode == HttpStatusCode.NotModified) return null;
        response.EnsureSuccessStatusCode();
        return await ReadAsync<CompiledPolicy>(response, cancellationToken);
    }

    public async Task<EventBatchResponse> UploadEventsAsync(AgentIdentity identity, IReadOnlyList<AgentEvent> events, CancellationToken cancellationToken)
    {
        using HttpResponseMessage response = await PostAsync("api/v1/agent/events", new EventBatchRequest(1, events), identity, cancellationToken);
        response.EnsureSuccessStatusCode();
        return await ReadAsync<EventBatchResponse>(response, cancellationToken);
    }

    public async Task<HeartbeatResponse> HeartbeatAsync(AgentIdentity identity, HeartbeatRequest heartbeat, CancellationToken cancellationToken)
    {
        using HttpResponseMessage response = await PostAsync("api/v1/agent/heartbeat", heartbeat, identity, cancellationToken);
        response.EnsureSuccessStatusCode();
        return await ReadAsync<HeartbeatResponse>(response, cancellationToken);
    }

    public async Task<AccessRequestResponse> RequestAccessAsync(AgentIdentity identity, AccessRequestCreate request, CancellationToken cancellationToken)
    {
        using HttpResponseMessage response = await PostAsync("api/v1/agent/requests", request, identity, cancellationToken);
        response.EnsureSuccessStatusCode();
        return await ReadAsync<AccessRequestResponse>(response, cancellationToken);
    }

    private async Task<HttpResponseMessage> PostAsync<T>(string path, T value, AgentIdentity? identity, CancellationToken cancellationToken)
    {
        HttpRequestMessage request = identity is null ? new(HttpMethod.Post, path) : Authorized(HttpMethod.Post, path, identity);
        request.Content = new StringContent(JsonSerializer.Serialize(value, PolicyJson.Options), Encoding.UTF8, "application/json");
        return await _httpClient.SendAsync(request, cancellationToken);
    }

    private static HttpRequestMessage Authorized(HttpMethod method, string path, AgentIdentity identity)
    {
        HttpRequestMessage request = new(method, path);
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", identity.ApiCredential);
        return request;
    }

    private static async Task<T> ReadAsync<T>(HttpResponseMessage response, CancellationToken cancellationToken)
    {
        await using Stream stream = await response.Content.ReadAsStreamAsync(cancellationToken);
        return await JsonSerializer.DeserializeAsync<T>(stream, PolicyJson.Options, cancellationToken) ?? throw new JsonException("Server returned JSON null.");
    }

    private bool ValidateCertificate(HttpRequestMessage _, System.Security.Cryptography.X509Certificates.X509Certificate2? certificate, System.Security.Cryptography.X509Certificates.X509Chain? __, SslPolicyErrors errors)
    {
        if (errors != SslPolicyErrors.None || certificate is null) return false;
        if (_options.CertificateSha256Pins.Length == 0) return true;
        string actual = Convert.ToHexString(SHA256.HashData(certificate.RawData));
        return _options.CertificateSha256Pins.Any(pin => string.Equals(pin.Replace(":", string.Empty, StringComparison.Ordinal), actual, StringComparison.OrdinalIgnoreCase));
    }

    public void Dispose() => _httpClient.Dispose();
    public static string AgentVersion => typeof(AgentApiClient).Assembly.GetName().Version?.ToString(3) ?? "1.0.0";
}
