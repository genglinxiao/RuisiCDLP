using System.Net;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Authorization.Infrastructure;
using Microsoft.AspNetCore.Mvc.Testing;
using Microsoft.AspNetCore.Routing;
using Microsoft.Data.Sqlite;
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
    [Trait("TestId", "T-M6-13")]
    [Trait("TestId", "T-M6-14")]
    [Trait("TestId", "T-M6-15")]
    [Trait("TestId", "T-M6-16")]
    [Trait("TestId", "T-M7-11")]
    [Trait("TestId", "T-M7-12")]
    [Trait("TestId", "T-M7-13")]
    [Trait("TestId", "T-M7-15")]
    public async Task InventoryNormalizesStrongIdentityAndIsolatesLocalIdentity()
    {
        EnrollmentResponse first = await EnrollAsync("WS-INVENTORY-A");
        EnrollmentResponse second = await EnrollAsync("WS-INVENTORY-B");
        DeviceSnapshot strong = new(@"USB\VID_1234&PID_5678\SERIAL-ONE", [], [], "1234", "5678", "SERIAL-ONE",
            "{guid}", "USBSTOR", "Factory transfer stick", "usb", ["usb_storage"], "Acme", "SecureKey 8", "USB storage device");
        await SendDeviceEventAsync(first, strong, "blocked", Decision.Block);
        await SendDeviceEventAsync(second, strong with { InstanceId = @"USB\VID_1234&PID_5678\SERIAL-ONE" }, "blocked", Decision.Block);
        DeviceSnapshot localA = strong with { InstanceId = @"USB\VID_1234&PID_5678\LOCAL-A", Serial = null, FriendlyName = "Local A" };
        DeviceSnapshot localB = localA with { InstanceId = @"USB\VID_1234&PID_5678\LOCAL-B", FriendlyName = "Local B" };
        await SendDeviceEventAsync(first, localA, "blocked", Decision.Block);
        await SendDeviceEventAsync(second, localB, "blocked", Decision.Block);
        await LoginAdminAsync();

        PagedResult<DeviceInventoryRow> acme = await ReadAsync<PagedResult<DeviceInventoryRow>>(await _client.GetAsync("/api/v1/admin/devices?q=acme"));
        Assert.Equal(3, acme.TotalCount);
        DeviceInventoryRow shared = Assert.Single(acme.Items, item => item.IdentityConfidence == "strong");
        Assert.Equal(2, shared.WorkstationCount);
        Assert.Equal("Acme", shared.Manufacturer);
        Assert.Equal(2, acme.Items.Count(item => item.IdentityConfidence == "local"));

        PagedResult<DeviceInventoryRow> byInstance = await ReadAsync<PagedResult<DeviceInventoryRow>>(await _client.GetAsync("/api/v1/admin/devices?q=LOCAL-B"));
        Assert.Equal("Local B", Assert.Single(byInstance.Items).FriendlyName);
        PagedResult<WorkstationInventoryRow> workstations = await ReadAsync<PagedResult<WorkstationInventoryRow>>(await _client.GetAsync("/api/v1/admin/workstations?q=INVENTORY-B"));
        Assert.Equal(second.WorkstationId, Assert.Single(workstations.Items).Id);
        string devicePage = WebUtility.HtmlDecode(await _client.GetStringAsync($"/?tab=devices&deviceId={shared.Id}&q=acme"));
        Assert.Contains("Factory transfer stick", devicePage, StringComparison.Ordinal);
        Assert.Contains("pair-card", devicePage, StringComparison.Ordinal);
        string workstationPage = WebUtility.HtmlDecode(await _client.GetStringAsync($"/?tab=workstations&workstationId={second.WorkstationId}&q=INVENTORY-B"));
        Assert.Contains("WS-INVENTORY-B", workstationPage, StringComparison.Ordinal);
        Assert.Contains("Observed USB devices", workstationPage, StringComparison.Ordinal);
        Assert.Equal(HttpStatusCode.BadRequest, (await _client.GetAsync("/api/v1/admin/devices?pageSize=13")).StatusCode);
    }

    [Fact]
    [Trait("TestId", "T-M6-17")]
    [Trait("TestId", "T-M6-19")]
    [Trait("TestId", "T-M6-20")]
    public async Task DirectPairGrantAndRevokePublishEffectiveStatus()
    {
        EnrollmentResponse enrollment = await EnrollAsync("WS-GRANT");
        DeviceSnapshot device = new(@"USB\VID_ABCD&PID_1234\PAIR-GRANT", [], [@"USB\VID_ABCD&PID_1234\PAIR-GRANT"],
            "ABCD", "1234", "PAIR-GRANT", "{guid}", "USBSTOR", "Grant target", "usb", ["usb_storage"], "Contoso", "Vault", "Test device");
        await SendDeviceEventAsync(enrollment, device, "blocked", Decision.Block);
        await LoginAdminAsync();
        DeviceInventoryRow inventory = Assert.Single((await ReadAsync<PagedResult<DeviceInventoryRow>>(await _client.GetAsync("/api/v1/admin/devices?q=PAIR-GRANT"))).Items);

        WorkstationDeviceRow before = await ReadAsync<WorkstationDeviceRow>(await _client.GetAsync($"/api/v1/admin/workstations/{enrollment.WorkstationId}/devices/{inventory.Id}"));
        Assert.Equal("block", before.EffectiveDecision);
        Assert.Equal("blocked", before.ObservedEnforcement);
        using HttpRequestMessage grant = new(HttpMethod.Post, $"/api/v1/admin/workstations/{enrollment.WorkstationId}/devices/{inventory.Id}/grants")
        {
            Content = Json(new { expiresAtUtc = DateTimeOffset.UtcNow.AddDays(7), reason = "Approved for integration validation" }),
        };
        grant.Headers.Add("X-Requested-With", "UsbDlp.Admin");
        using HttpResponseMessage grantedResponse = await _client.SendAsync(grant);
        Assert.Equal(HttpStatusCode.Created, grantedResponse.StatusCode);
        GrantResponse granted = await ReadAsync<GrantResponse>(grantedResponse);
        Assert.Equal("allow", granted.Pair.EffectiveDecision);
        Assert.True(granted.Pair.CanRevoke);

        using HttpRequestMessage revoke = new(HttpMethod.Delete, $"/api/v1/admin/workstations/{enrollment.WorkstationId}/devices/{inventory.Id}/grants/{granted.EntryId}");
        revoke.Headers.Add("X-Requested-With", "UsbDlp.Admin");
        Assert.Equal(HttpStatusCode.NoContent, (await _client.SendAsync(revoke)).StatusCode);
        WorkstationDeviceRow after = await ReadAsync<WorkstationDeviceRow>(await _client.GetAsync($"/api/v1/admin/workstations/{enrollment.WorkstationId}/devices/{inventory.Id}"));
        Assert.Equal("block", after.EffectiveDecision);
        Assert.Equal(3, after.PolicyVersion);

        using HttpRequestMessage describe = new(HttpMethod.Put, $"/api/v1/admin/devices/{inventory.Id}/description") { Content = Json(new { description = "Lab firmware transfer media" }) };
        describe.Headers.Add("X-Requested-With", "UsbDlp.Admin");
        Assert.Equal(HttpStatusCode.NoContent, (await _client.SendAsync(describe)).StatusCode);
        PagedResult<DeviceInventoryRow> described = await ReadAsync<PagedResult<DeviceInventoryRow>>(await _client.GetAsync("/api/v1/admin/devices?q=firmware"));
        Assert.Equal(inventory.Id, Assert.Single(described.Items).Id);
    }

    [Fact]
    [Trait("TestId", "T-M6-18")]
    public async Task InitializationBackfillsUnlinkedLegacyDeviceSnapshotsIdempotently()
    {
        EnrollmentResponse enrollment = await EnrollAsync("WS-BACKFILL");
        DeviceSnapshot device = new(@"USB\VID_1111&PID_2222\BACKFILL", [], [], "1111", "2222", "BACKFILL",
            "{guid}", "USBSTOR", "Legacy snapshot", "usb", ["usb_storage"], "Legacy Corp", "Archive", "Imported record");
        await SendDeviceEventAsync(enrollment, device, "blocked", Decision.Block);
        await using (SqliteConnection connection = new($"Data Source={_databasePath};Pooling=False"))
        {
            await connection.OpenAsync();
            await using SqliteCommand reset = connection.CreateCommand();
            reset.CommandText = "UPDATE events SET usb_device_id=NULL,workstation_device_id=NULL; DELETE FROM workstation_devices; DELETE FROM usb_devices;";
            await reset.ExecuteNonQueryAsync();
        }
        UsbDlpRepository repository = _factory.Services.GetRequiredService<UsbDlpRepository>();
        await repository.InitializeAsync(null, CancellationToken.None);
        await repository.InitializeAsync(null, CancellationToken.None);
        await LoginAdminAsync();
        PagedResult<DeviceInventoryRow> result = await ReadAsync<PagedResult<DeviceInventoryRow>>(await _client.GetAsync("/api/v1/admin/devices?q=BACKFILL"));
        Assert.Single(result.Items);
        Assert.Equal(1, result.TotalCount);
        Assert.Equal(1, Assert.Single(result.Items).WorkstationCount);
    }

    [Fact]
    [Trait("TestId", "T-M6-18")]
    public async Task PreInventoryDatabaseMigratesWithoutRecreation()
    {
        DirectoryInfo? root = new(AppContext.BaseDirectory);
        while (root is not null && !File.Exists(Path.Combine(root.FullName, "UsbDlp.sln"))) root = root.Parent;
        Assert.NotNull(root);
        string source = Path.Combine(root.FullName, "src", "UsbDlp.Server", "App_Data", "smoke.db");
        Assert.True(File.Exists(source));
        string migrated = Path.Combine(Path.GetTempPath(), $"usb-dlp-legacy-{Guid.NewGuid():N}.db");
        File.Copy(source, migrated);
        try
        {
            await using WebApplicationFactory<Program> legacyFactory = new WebApplicationFactory<Program>().WithWebHostBuilder(builder =>
            {
                builder.UseEnvironment("Development");
                builder.UseSetting("UsbDlp:DatabasePath", migrated);
                builder.UseSetting("UsbDlp:BootstrapAdminUsername", "admin");
                builder.UseSetting("UsbDlp:BootstrapAdminPassword", "Integration-Admin-Password!");
            });
            _ = legacyFactory.CreateClient();
            _ = legacyFactory.Services.GetRequiredService<UsbDlpRepository>();
            await using SqliteConnection connection = new($"Data Source={migrated};Pooling=False");
            await connection.OpenAsync();
            Assert.True(await TableExistsAsync(connection, "usb_devices"));
            Assert.True(await TableExistsAsync(connection, "workstation_devices"));
            Assert.True(await ColumnExistsAsync(connection, "access_requests", "workstation_device_id"));
            Assert.True(await ColumnExistsAsync(connection, "events", "usb_device_id"));
        }
        finally
        {
            foreach (string path in new[] { migrated, migrated + "-shm", migrated + "-wal" }) if (File.Exists(path)) File.Delete(path);
        }
    }

    [Fact]
    [Trait("TestId", "T-M7-14")]
    public void EveryAdminMutationRequiresAdministratorRole()
    {
        EndpointDataSource endpoints = _factory.Services.GetRequiredService<EndpointDataSource>();
        RouteEndpoint[] mutations = endpoints.Endpoints.OfType<RouteEndpoint>()
            .Where(endpoint => endpoint.RoutePattern.RawText?.StartsWith("/api/v1/admin", StringComparison.Ordinal) == true &&
                endpoint.Metadata.GetMetadata<HttpMethodMetadata>()?.HttpMethods.All(method => method is not "GET" and not "HEAD") == true)
            .ToArray();
        Assert.NotEmpty(mutations);
        Assert.All(mutations, endpoint => Assert.Contains(
            endpoint.Metadata.GetMetadata<AuthorizationPolicy>()?.Requirements.OfType<RolesAuthorizationRequirement>() ?? [],
            requirement => requirement.AllowedRoles.Contains("admin", StringComparer.OrdinalIgnoreCase)));
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

    private Task<EnrollmentResponse> EnrollAsync() => EnrollAsync("WS-TEST");

    private async Task<EnrollmentResponse> EnrollAsync(string hostname)
    {
        EnrollmentRequest request = new(1, EnrollmentToken, hostname, $"S-1-5-21-{Guid.NewGuid():N}", "1.0.0", Guid.NewGuid());
        using HttpResponseMessage response = await PostAsync("/api/v1/agent/enroll", request);
        response.EnsureSuccessStatusCode();
        return await ReadAsync<EnrollmentResponse>(response);
    }

    private async Task SendDeviceEventAsync(EnrollmentResponse enrollment, DeviceSnapshot device, string type, Decision decision)
    {
        AgentEvent item = new(Guid.NewGuid(), type, DateTimeOffset.UtcNow, "WS-TEST", null, null, device,
            1, decision, null, "default_deny", "1.0.0", JsonSerializer.SerializeToElement(new { }));
        await PostAuthorizedAsync<EventBatchRequest, EventBatchResponse>("/api/v1/agent/events", new(1, [item]), enrollment.ApiCredential);
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

    private static async Task<bool> TableExistsAsync(SqliteConnection connection, string table)
    {
        await using SqliteCommand command = connection.CreateCommand(); command.CommandText = "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=$name;";
        command.Parameters.AddWithValue("$name", table); return Convert.ToInt32(await command.ExecuteScalarAsync(), System.Globalization.CultureInfo.InvariantCulture) == 1;
    }

    private static async Task<bool> ColumnExistsAsync(SqliteConnection connection, string table, string column)
    {
        await using SqliteCommand command = connection.CreateCommand(); command.CommandText = $"PRAGMA table_info({table});";
        await using SqliteDataReader reader = await command.ExecuteReaderAsync(); while (await reader.ReadAsync()) if (reader.GetString(1) == column) return true;
        return false;
    }
}
