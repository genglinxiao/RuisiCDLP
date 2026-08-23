using System.Net;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Mvc.Testing;
using Microsoft.Extensions.DependencyInjection;
using UsbDlp.Contracts;
using UsbDlp.Policy;
using UsbDlp.Server.Data;

namespace UsbDlp.IntegrationTests;

public sealed class ServerApiTests : IAsyncLifetime
{
    private readonly string _databasePath = Path.Combine(Path.GetTempPath(), $"usb-dlp-tests-{Guid.NewGuid():N}.db");
    private WebApplicationFactory<Program> _factory = null!;
    private HttpClient _client = null!;
    private const string EnrollmentToken = "integration-enrollment-token-32-bytes";

    public Task InitializeAsync()
    {
        _factory = new WebApplicationFactory<Program>().WithWebHostBuilder(builder =>
        {
            builder.UseEnvironment("Development");
            builder.UseSetting("UsbDlp:DatabasePath", _databasePath);
            builder.UseSetting("UsbDlp:BootstrapEnrollmentToken", EnrollmentToken);
            builder.UseSetting("UsbDlp:PolicyPollSeconds", "15");
            builder.UseSetting("UsbDlp:BootstrapAdminUsername", "admin");
            builder.UseSetting("UsbDlp:BootstrapAdminPassword", "Integration-Admin-Password!");
        });
        _client = _factory.CreateClient();
        return Task.CompletedTask;
    }

    public async Task DisposeAsync()
    {
        _client.Dispose();
        await _factory.DisposeAsync();
        foreach (string path in new[] { _databasePath, _databasePath + "-shm", _databasePath + "-wal" })
            if (File.Exists(path)) File.Delete(path);
    }

    [Fact]
    [Trait("TestId", "T-M6-01")]
    public async Task EnrollmentReturnsCredentialAndAuthenticatedPolicy()
    {
        EnrollmentResponse enrollment = await EnrollAsync();
        Assert.NotEqual(Guid.Empty, enrollment.WorkstationId);
        Assert.StartsWith(enrollment.WorkstationId.ToString(), enrollment.ApiCredential, StringComparison.Ordinal);

        using HttpRequestMessage policyRequest = Authorized(HttpMethod.Get, "/api/v1/agent/policy", enrollment.ApiCredential);
        using HttpResponseMessage response = await _client.SendAsync(policyRequest);
        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        CompiledPolicy policy = await ReadAsync<CompiledPolicy>(response);
        Assert.Equal(GroupMode.DefaultDeny, policy.GroupMode);
        Assert.Equal(enrollment.WorkstationId, policy.WorkstationId);

        using HttpRequestMessage unauthorized = Authorized(HttpMethod.Get, "/api/v1/agent/policy", enrollment.ApiCredential + "wrong");
        Assert.Equal(HttpStatusCode.Unauthorized, (await _client.SendAsync(unauthorized)).StatusCode);
    }

    [Fact]
    [Trait("TestId", "T-M6-05")]
    public async Task DuplicateEventIsAcceptedIdempotently()
    {
        EnrollmentResponse enrollment = await EnrollAsync();
        AgentEvent item = new(
            Guid.NewGuid(), "agent_start", DateTimeOffset.UtcNow, "WS-TEST", null, null, null,
            1, null, null, null, "1.0.0", JsonSerializer.SerializeToElement(new { }));
        EventBatchRequest batch = new(1, [item]);

        EventBatchResponse first = await PostAuthorizedAsync<EventBatchRequest, EventBatchResponse>("/api/v1/agent/events", batch, enrollment.ApiCredential);
        EventBatchResponse second = await PostAuthorizedAsync<EventBatchRequest, EventBatchResponse>("/api/v1/agent/events", batch, enrollment.ApiCredential);
        Assert.Contains(item.ClientEventId, first.AcceptedClientEventIds);
        Assert.Contains(item.ClientEventId, second.DuplicateClientEventIds);
    }

    [Fact]
    [Trait("TestId", "T-M6-11")]
    public async Task RequestReplayReturnsSameRequest()
    {
        EnrollmentResponse enrollment = await EnrollAsync();
        DeviceSnapshot device = new(@"USB\VID_0781&PID_5583\SERIAL", [], [], "0781", "5583", "SERIAL", "{guid}", "USBSTOR", "Test stick", "usb", ["usb_storage"]);
        AccessRequestCreate request = new(1, Guid.NewGuid(), DateTimeOffset.UtcNow, "S-1-5-21-1", "DOMAIN\\user", device, 7, "Required for integration testing.");
        AccessRequestResponse first = await PostAuthorizedAsync<AccessRequestCreate, AccessRequestResponse>("/api/v1/agent/requests", request, enrollment.ApiCredential);
        AccessRequestResponse second = await PostAuthorizedAsync<AccessRequestCreate, AccessRequestResponse>("/api/v1/agent/requests", request, enrollment.ApiCredential);
        Assert.Equal(first.RequestId, second.RequestId);
        Assert.Equal("pending", first.Status);
    }

    [Fact]
    public async Task RecentRequestForSameDeviceReturnsCooldownInsteadOfConflict()
    {
        EnrollmentResponse enrollment = await EnrollAsync();
        DeviceSnapshot device = new(@"USB\VID_0781&PID_5583\COOLDOWN", [], [], "0781", "5583", "COOLDOWN", "{guid}", "USBSTOR", "Cooldown stick", "usb", ["usb_storage"]);
        AccessRequestCreate firstRequest = new(1, Guid.NewGuid(), DateTimeOffset.UtcNow, "S-1-5-21-1", "DOMAIN\\user", device, 7, "First cooldown integration request.");
        AccessRequestCreate secondRequest = firstRequest with { ClientRequestId = Guid.NewGuid(), Justification = "Second cooldown integration request." };

        AccessRequestResponse first = await PostAuthorizedAsync<AccessRequestCreate, AccessRequestResponse>("/api/v1/agent/requests", firstRequest, enrollment.ApiCredential);
        AccessRequestResponse second = await PostAuthorizedAsync<AccessRequestCreate, AccessRequestResponse>("/api/v1/agent/requests", secondRequest, enrollment.ApiCredential);

        Assert.Equal(first.RequestId, second.RequestId);
        Assert.NotNull(first.RetryAfterUtc);
        Assert.Equal("recent", second.Status);
        Assert.NotNull(second.RetryAfterUtc);
        Assert.True(second.RetryAfterUtc > DateTimeOffset.UtcNow.AddMinutes(9));
    }

    [Fact]
    [Trait("TestId", "T-M6-06")]
    [Trait("TestId", "T-M7-01")]
    public async Task AdminLoginPublishesPolicyVisibleToAgent()
    {
        EnrollmentResponse enrollment = await EnrollAsync();
        using HttpResponseMessage loginPage = await _client.GetAsync("/Login");
        string html = await loginPage.Content.ReadAsStringAsync();
        string token = Regex.Match(html, "name=\"__RequestVerificationToken\"[^>]*value=\"([^\"]+)\"").Groups[1].Value;
        Assert.NotEmpty(token);
        using FormUrlEncodedContent login = new(new Dictionary<string, string>
        {
            ["Username"] = "admin", ["Password"] = "Integration-Admin-Password!", ["__RequestVerificationToken"] = token,
        });
        using HttpResponseMessage signedIn = await _client.PostAsync("/Login", login);
        Assert.Equal(HttpStatusCode.OK, signedIn.StatusCode);

        var body = new { displayName = "Integration block", matchType = "vid_pid", vid = "0781", pid = "5583", serial = (string?)null, instanceId = (string?)null, deviceClass = (string?)null, action = "block", scopeType = "global", scopeId = (Guid?)null, mandatory = false, expiresAtUtc = (DateTimeOffset?)null };
        using HttpRequestMessage add = new(HttpMethod.Post, "/api/v1/admin/policies") { Content = Json(body) };
        add.Headers.Add("X-Requested-With", "UsbDlp.Admin");
        using HttpResponseMessage added = await _client.SendAsync(add);
        Assert.Equal(HttpStatusCode.Created, added.StatusCode);

        using HttpRequestMessage policyRequest = Authorized(HttpMethod.Get, "/api/v1/agent/policy", enrollment.ApiCredential);
        CompiledPolicy policy = await ReadAsync<CompiledPolicy>(await _client.SendAsync(policyRequest));
        Assert.Equal(2, policy.PolicyVersion);
        Assert.Contains(policy.Entries, entry => entry.DisplayName == "Integration block" && entry.Action == EntryAction.Block);
    }

    [Fact]
    public async Task AdminCanDeletePolicyAndPublishNewVersion()
    {
        EnrollmentResponse enrollment = await EnrollAsync();
        await LoginAdminAsync();
        var body = new { displayName = "Temporary policy", matchType = "vid_pid", vid = "0781", pid = "5583", serial = (string?)null, instanceId = (string?)null, deviceClass = (string?)null, action = "block", scopeType = "global", scopeId = (Guid?)null, mandatory = false, expiresAtUtc = (DateTimeOffset?)null };
        using HttpRequestMessage add = new(HttpMethod.Post, "/api/v1/admin/policies") { Content = Json(body) };
        add.Headers.Add("X-Requested-With", "UsbDlp.Admin");
        using HttpResponseMessage added = await _client.SendAsync(add);
        Guid id = JsonDocument.Parse(await added.Content.ReadAsStringAsync()).RootElement.GetProperty("id").GetGuid();

        using HttpRequestMessage delete = new(HttpMethod.Delete, $"/api/v1/admin/policies/{id}");
        delete.Headers.Add("X-Requested-With", "UsbDlp.Admin");
        Assert.Equal(HttpStatusCode.NoContent, (await _client.SendAsync(delete)).StatusCode);

        using HttpRequestMessage policyRequest = Authorized(HttpMethod.Get, "/api/v1/agent/policy", enrollment.ApiCredential);
        CompiledPolicy policy = await ReadAsync<CompiledPolicy>(await _client.SendAsync(policyRequest));
        Assert.Equal(3, policy.PolicyVersion);
        Assert.DoesNotContain(policy.Entries, entry => entry.Id == id);
    }

    [Fact]
    [Trait("TestId", "T-M7-03")]
    public async Task ApprovedRequestCreatesExpiringWorkstationAllow()
    {
        EnrollmentResponse enrollment = await EnrollAsync();
        DeviceSnapshot device = new(@"USB\VID_0781&PID_5583\APPROVE", [@"USB\VID_0781&PID_5583"], [@"USB\VID_0781&PID_5583\APPROVE"], "0781", "5583", "APPROVE", "{guid}", "USBSTOR", "Approved test stick", "usb", ["usb_storage"]);
        AccessRequestCreate create = new(1, Guid.NewGuid(), DateTimeOffset.UtcNow, "S-1-5-21-1", "DOMAIN\\user", device, 7, "Required to validate approval convergence.");
        AccessRequestResponse requested = await PostAuthorizedAsync<AccessRequestCreate, AccessRequestResponse>("/api/v1/agent/requests", create, enrollment.ApiCredential);
        await LoginAdminAsync();

        using HttpRequestMessage approve = new(HttpMethod.Post, $"/api/v1/admin/requests/{requested.RequestId}/approve")
        {
            Content = Json(new { durationDays = 7, reason = "Approved for test" }),
        };
        approve.Headers.Add("X-Requested-With", "UsbDlp.Admin");
        Assert.Equal(HttpStatusCode.OK, (await _client.SendAsync(approve)).StatusCode);

        using HttpRequestMessage policyRequest = Authorized(HttpMethod.Get, "/api/v1/agent/policy", enrollment.ApiCredential);
        CompiledPolicy policy = await ReadAsync<CompiledPolicy>(await _client.SendAsync(policyRequest));
        DeviceEntry entry = Assert.Single(policy.Entries);
        Assert.Equal(EntryAction.Allow, entry.Action);
        Assert.Equal(ScopeType.Workstation, entry.ScopeType);
        Assert.Equal(enrollment.WorkstationId, entry.ScopeId);
        Assert.Contains(device.InstanceId, entry.DeviceInstallIds);
        Assert.True(entry.ExpiresAtUtc > DateTimeOffset.UtcNow.AddDays(6));

        using HttpRequestMessage revoke = new(HttpMethod.Post, $"/api/v1/admin/requests/{requested.RequestId}/revoke")
        {
            Content = Json(new { }),
        };
        revoke.Headers.Add("X-Requested-With", "UsbDlp.Admin");
        Assert.Equal(HttpStatusCode.OK, (await _client.SendAsync(revoke)).StatusCode);

        using HttpRequestMessage revokedPolicyRequest = Authorized(HttpMethod.Get, "/api/v1/agent/policy", enrollment.ApiCredential);
        CompiledPolicy revokedPolicy = await ReadAsync<CompiledPolicy>(await _client.SendAsync(revokedPolicyRequest));
        Assert.Empty(revokedPolicy.Entries);
        Assert.Equal(3, revokedPolicy.PolicyVersion);

        DashboardModel dashboard = await ReadAsync<DashboardModel>(await _client.GetAsync("/api/v1/admin/dashboard"));
        Assert.Equal("revoked", Assert.Single(dashboard.Requests).Status);
    }

    [Fact]
    [Trait("TestId", "T-M8-04")]
    public async Task CultureSwitchLocalizesLoginAndPersistsCookie()
    {
        System.Globalization.CultureInfo previous = System.Globalization.CultureInfo.CurrentUICulture;
        System.Globalization.CultureInfo.CurrentUICulture = System.Globalization.CultureInfo.GetCultureInfo("zh-CN");
        try
        {
            Assert.Equal("USB 设备管控", _factory.Services.GetRequiredService<UsbDlp.Server.SharedResource>()["USB device control"]);
        }
        finally { System.Globalization.CultureInfo.CurrentUICulture = previous; }
        using HttpResponseMessage response = await _client.GetAsync("/culture/zh-CN?returnUrl=/Login");
        string html = WebUtility.HtmlDecode(await response.Content.ReadAsStringAsync());
        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        Assert.Contains("USB 设备管控", html, StringComparison.Ordinal);
        Assert.Contains("管理员控制台", html, StringComparison.Ordinal);

        string persisted = WebUtility.HtmlDecode(await _client.GetStringAsync("/Login"));
        Assert.Contains("用户名", persisted, StringComparison.Ordinal);
    }

    private async Task<EnrollmentResponse> EnrollAsync()
    {
        EnrollmentRequest request = new(1, EnrollmentToken, "WS-TEST", $"S-1-5-21-{Guid.NewGuid():N}", "1.0.0", Guid.NewGuid());
        using HttpResponseMessage response = await PostAsync("/api/v1/agent/enroll", request);
        response.EnsureSuccessStatusCode();
        return await ReadAsync<EnrollmentResponse>(response);
    }

    private async Task LoginAdminAsync()
    {
        using HttpResponseMessage loginPage = await _client.GetAsync("/Login");
        string html = await loginPage.Content.ReadAsStringAsync();
        string token = Regex.Match(html, "name=\"__RequestVerificationToken\"[^>]*value=\"([^\"]+)\"").Groups[1].Value;
        using FormUrlEncodedContent login = new(new Dictionary<string, string>
        {
            ["Username"] = "admin", ["Password"] = "Integration-Admin-Password!", ["__RequestVerificationToken"] = token,
        });
        (await _client.PostAsync("/Login", login)).EnsureSuccessStatusCode();
    }

    private async Task<TResponse> PostAuthorizedAsync<TRequest, TResponse>(string path, TRequest body, string credential)
    {
        using HttpRequestMessage request = Authorized(HttpMethod.Post, path, credential);
        request.Content = Json(body);
        using HttpResponseMessage response = await _client.SendAsync(request);
        response.EnsureSuccessStatusCode();
        return await ReadAsync<TResponse>(response);
    }

    private Task<HttpResponseMessage> PostAsync<T>(string path, T body) => _client.PostAsync(path, Json(body));
    private static StringContent Json<T>(T body) => new(JsonSerializer.Serialize(body, PolicyJson.Options), Encoding.UTF8, "application/json");
    private static HttpRequestMessage Authorized(HttpMethod method, string path, string credential)
    {
        HttpRequestMessage request = new(method, path);
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", credential);
        return request;
    }
    private static async Task<T> ReadAsync<T>(HttpResponseMessage response) => JsonSerializer.Deserialize<T>(await response.Content.ReadAsStringAsync(), PolicyJson.Options)!;
}
