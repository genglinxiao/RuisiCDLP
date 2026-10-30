using Microsoft.AspNetCore.Authentication.Cookies;
using Microsoft.AspNetCore.RateLimiting;
using Microsoft.AspNetCore.Localization;
using System.Globalization;
using UsbDlp.Contracts;
using UsbDlp.Policy;
using UsbDlp.Server;
using UsbDlp.Server.Data;

WebApplicationBuilder builder = WebApplication.CreateBuilder(args);
builder.Services.AddWindowsService(options => options.ServiceName = "Ruisi USB DLP Server");
BootstrapConfiguration.LoadFileSecrets(builder.Configuration);
bool allowInsecureHttp = builder.Configuration.GetValue("UsbDlp:AllowInsecureHttp", false);
builder.Services.ConfigureHttpJsonOptions(options =>
{
    options.SerializerOptions.PropertyNamingPolicy = PolicyJson.Options.PropertyNamingPolicy;
    foreach (System.Text.Json.Serialization.JsonConverter converter in PolicyJson.Options.Converters)
        options.SerializerOptions.Converters.Add(converter);
});
builder.Services.AddSingleton<UsbDlpRepository>();
builder.Services.AddRateLimiter(options => options.AddFixedWindowLimiter("enrollment", limiter =>
{
    limiter.PermitLimit = 10;
    limiter.Window = TimeSpan.FromMinutes(15);
}));
builder.Services.AddAuthentication(CookieAuthenticationDefaults.AuthenticationScheme).AddCookie(options =>
{
    options.LoginPath = "/Login";
    options.AccessDeniedPath = "/Login";
    options.Cookie.Name = "UsbDlp.Admin";
    options.Cookie.HttpOnly = true;
    options.Cookie.SameSite = SameSiteMode.Lax;
    options.Cookie.SecurePolicy = builder.Environment.IsDevelopment() || allowInsecureHttp ? CookieSecurePolicy.SameAsRequest : CookieSecurePolicy.Always;
    options.ExpireTimeSpan = TimeSpan.FromMinutes(30);
    options.SlidingExpiration = true;
});
builder.Services.AddAuthorization();
builder.Services.AddHealthChecks();
builder.Services.AddLocalization(options => options.ResourcesPath = "Resources");
builder.Services.AddSingleton<UsbDlp.Server.SharedResource>();
builder.Services.AddRazorPages(options => options.Conventions.AuthorizeFolder("/").AllowAnonymousToPage("/Login"));

WebApplication app = builder.Build();
UsbDlpRepository repository = app.Services.GetRequiredService<UsbDlpRepository>();
await repository.InitializeAsync(builder.Configuration["UsbDlp:BootstrapEnrollmentToken"], CancellationToken.None);

if (!app.Environment.IsDevelopment())
{
    app.UseExceptionHandler("/Error");
    if (!allowInsecureHttp)
    {
        app.UseHsts();
        app.UseHttpsRedirection();
    }
}
app.Use(async (context, next) =>
{
    string? supplied = context.Request.Headers["X-Correlation-ID"].FirstOrDefault();
    context.Response.Headers["X-Correlation-ID"] = supplied is not null && Guid.TryParse(supplied, out _) ? supplied : Guid.NewGuid().ToString();
    await next(context);
});
app.UseStaticFiles();
app.UseRouting();
CultureInfo[] supportedCultures = [new("en-US"), new("zh-CN")];
app.UseRequestLocalization(new RequestLocalizationOptions
{
    DefaultRequestCulture = new RequestCulture("en-US"),
    SupportedCultures = supportedCultures,
    SupportedUICultures = supportedCultures
}.AddInitialRequestCultureProvider(new CookieRequestCultureProvider()));
app.UseRateLimiter();
app.UseAuthentication();
app.UseAuthorization();
app.Use(async (context, next) =>
{
    bool adminMutation = context.Request.Path.StartsWithSegments("/api/v1/admin") &&
        !HttpMethods.IsGet(context.Request.Method) && !HttpMethods.IsHead(context.Request.Method);
    if (adminMutation && !string.Equals(context.Request.Headers["X-Requested-With"], "UsbDlp.Admin", StringComparison.Ordinal))
    {
        context.Response.StatusCode = StatusCodes.Status400BadRequest;
        await context.Response.WriteAsJsonAsync(new { title = "Missing same-origin request proof", status = 400 });
        return;
    }
    await next(context);
});

RouteGroupBuilder agentApi = app.MapGroup("/api/v1/agent");
agentApi.MapPost("/enroll", async (EnrollmentRequest request, UsbDlpRepository store, IConfiguration config, CancellationToken token) =>
{
    bool validPlatform = request.SchemaVersion == 1 ||
        request.SchemaVersion == 2 &&
        request.OsVersion is { Length: >= 1 and <= 64 } && request.OsVersion.All(character => character is >= ' ' and <= '~') &&
        request.Architecture is "x86" or "x64" &&
        request.ImplementationProfile is "legacy-xp-x86" or "legacy-win7-x86" or "legacy-win7-x64" or "modern-win10-x86" or "modern-win10-x64" &&
        (request.ImplementationProfile.EndsWith("-x86", StringComparison.Ordinal) ? request.Architecture == "x86" : request.Architecture == "x64");
    if (!validPlatform || request.ClientNonce == Guid.Empty || string.IsNullOrWhiteSpace(request.Hostname) || request.Hostname.Length > 63 || string.IsNullOrWhiteSpace(request.MachineSid))
        return Results.ValidationProblem(new Dictionary<string, string[]> { ["request"] = ["Enrollment fields are invalid."] });
    int pollSeconds = config.GetValue("UsbDlp:PolicyPollSeconds", 30);
    (EnrollmentResponse Response, bool Created)? result = await store.EnrollAsync(request, pollSeconds, token);
    return result is null ? Results.Problem(statusCode: 403, title: "Invalid enrollment token") : Results.Json(result.Value.Response, PolicyJson.Options, statusCode: result.Value.Created ? 201 : 200);
}).AllowAnonymous().RequireRateLimiting("enrollment");

agentApi.MapGet("/policy", async (HttpContext context, UsbDlpRepository store, CancellationToken token) =>
{
    AuthenticatedAgent? agent = await AuthenticateAsync(context, store, token);
    if (agent is null) return Results.Unauthorized();
    CompiledPolicy policy = await store.CompilePolicyAsync(agent, token);
    string etag = $"\"{policy.PolicyVersion}\"";
    context.Response.Headers.ETag = etag;
    context.Response.Headers.CacheControl = "no-store";
    return context.Request.Headers.IfNoneMatch.Contains(etag) ? Results.StatusCode(304) : Results.Json(policy, PolicyJson.Options);
});

agentApi.MapPost("/events", async (HttpContext context, EventBatchRequest request, UsbDlpRepository store, CancellationToken token) =>
{
    AuthenticatedAgent? agent = await AuthenticateAsync(context, store, token);
    if (agent is null) return Results.Unauthorized();
    if (request.SchemaVersion != 1 || request.Events.Count is < 1 or > 500)
        return Results.ValidationProblem(new Dictionary<string, string[]> { ["events"] = ["Submit 1 to 500 events."] });
    return Results.Json(await store.AddEventsAsync(agent, request, token), PolicyJson.Options);
});

agentApi.MapPost("/requests", async (HttpContext context, AccessRequestCreate request, UsbDlpRepository store, CancellationToken token) =>
{
    AuthenticatedAgent? agent = await AuthenticateAsync(context, store, token);
    if (agent is null) return Results.Unauthorized();
    if (request.SchemaVersion != 1 || request.RequestedDurationDays is not (1 or 7 or 30 or 90) || request.Justification.Trim().Length is < 10 or > 2000)
        return Results.ValidationProblem(new Dictionary<string, string[]> { ["request"] = ["Duration or justification is invalid."] });
    (AccessRequestResponse response, bool created, _) = await store.AddRequestAsync(agent, request, token);
    return Results.Json(response, PolicyJson.Options, statusCode: created ? 201 : 200);
});

agentApi.MapPost("/heartbeat", async (HttpContext context, HeartbeatRequest request, UsbDlpRepository store, CancellationToken token) =>
{
    AuthenticatedAgent? agent = await AuthenticateAsync(context, store, token);
    if (agent is null) return Results.Unauthorized();
    long current = await store.HeartbeatAsync(agent, request, token);
    return Results.Json(new HeartbeatResponse(1, DateTimeOffset.UtcNow, current != request.AppliedPolicyVersion, current), PolicyJson.Options);
});

RouteGroupBuilder adminApi = app.MapGroup("/api/v1/admin").RequireAuthorization();
adminApi.MapGet("/dashboard", async (UsbDlpRepository store, CancellationToken token) => Results.Json(await store.DashboardAsync(token), PolicyJson.Options));
adminApi.MapGet("/search", async (string? q, string? kind, int? page, int? pageSize, UsbDlpRepository store, CancellationToken token) =>
{
    string selectedKind = kind ?? "all"; int selectedPage = page ?? 1; int selectedSize = pageSize ?? 25;
    if (selectedKind is not ("all" or "device" or "workstation") || !ValidPage(selectedPage, selectedSize) || !ValidQuery(q))
        return Results.ValidationProblem(new Dictionary<string, string[]> { ["query"] = ["Search parameters are invalid."] });
    return Results.Json(await store.SearchInventoryAsync(q, selectedKind, selectedPage, selectedSize, token), PolicyJson.Options);
});
adminApi.MapGet("/devices", async (string? q, int? page, int? pageSize, string? sort, UsbDlpRepository store, CancellationToken token) =>
{
    int selectedPage = page ?? 1; int selectedSize = pageSize ?? 25; string selectedSort = sort ?? "lastSeen";
    if (!ValidPage(selectedPage, selectedSize) || !ValidQuery(q) || selectedSort is not ("lastSeen" or "name" or "manufacturer" or "model"))
        return Results.ValidationProblem(new Dictionary<string, string[]> { ["query"] = ["Device search parameters are invalid."] });
    return Results.Json(await store.SearchDevicesAsync(q, selectedPage, selectedSize, selectedSort, token), PolicyJson.Options);
});
adminApi.MapGet("/devices/{id:guid}", async (Guid id, UsbDlpRepository store, CancellationToken token) =>
    await store.DeviceDetailAsync(id, token) is { } detail ? Results.Json(detail, PolicyJson.Options) : Results.NotFound());
adminApi.MapPut("/devices/{id:guid}/description", async (Guid id, DescriptionRequest request, HttpContext context, UsbDlpRepository store, CancellationToken token) =>
{
    try { return await store.UpdateDescriptionAsync("device", id, request.Description, context.User.Identity?.Name ?? "admin", token) ? Results.NoContent() : Results.NotFound(); }
    catch (ArgumentException exception) { return Results.ValidationProblem(new Dictionary<string, string[]> { ["description"] = [exception.Message] }); }
}).RequireAuthorization(policy => policy.RequireRole("admin"));
adminApi.MapGet("/workstations", async (string? q, int? page, int? pageSize, string? sort, UsbDlpRepository store, CancellationToken token) =>
{
    int selectedPage = page ?? 1; int selectedSize = pageSize ?? 25; string selectedSort = sort ?? "lastSeen";
    if (!ValidPage(selectedPage, selectedSize) || !ValidQuery(q) || selectedSort is not ("lastSeen" or "hostname"))
        return Results.ValidationProblem(new Dictionary<string, string[]> { ["query"] = ["Workstation search parameters are invalid."] });
    return Results.Json(await store.SearchWorkstationsAsync(q, selectedPage, selectedSize, selectedSort, token), PolicyJson.Options);
});
adminApi.MapGet("/workstations/{id:guid}", async (Guid id, UsbDlpRepository store, CancellationToken token) =>
    await store.WorkstationDetailAsync(id, token) is { } detail ? Results.Json(detail, PolicyJson.Options) : Results.NotFound());
adminApi.MapPut("/workstations/{id:guid}/description", async (Guid id, DescriptionRequest request, HttpContext context, UsbDlpRepository store, CancellationToken token) =>
{
    try { return await store.UpdateDescriptionAsync("workstation", id, request.Description, context.User.Identity?.Name ?? "admin", token) ? Results.NoContent() : Results.NotFound(); }
    catch (ArgumentException exception) { return Results.ValidationProblem(new Dictionary<string, string[]> { ["description"] = [exception.Message] }); }
}).RequireAuthorization(policy => policy.RequireRole("admin"));
adminApi.MapGet("/workstations/{workstationId:guid}/devices/{deviceId:guid}", async (Guid workstationId, Guid deviceId, UsbDlpRepository store, CancellationToken token) =>
    await store.PairDetailAsync(workstationId, deviceId, token) is { } pair ? Results.Json(pair, PolicyJson.Options) : Results.NotFound());
adminApi.MapPost("/workstations/{workstationId:guid}/devices/{deviceId:guid}/grants", async (Guid workstationId, Guid deviceId, GrantRequest request, HttpContext context, UsbDlpRepository store, CancellationToken token) =>
{
    try
    {
        GrantResponse? result = await store.AddPairGrantAsync(workstationId, deviceId, request, context.User.Identity?.Name ?? "admin", token);
        return result is null ? Results.NotFound() : Results.Created($"/api/v1/admin/workstations/{workstationId}/devices/{deviceId}", result);
    }
    catch (ArgumentException exception) { return Results.ValidationProblem(new Dictionary<string, string[]> { ["grant"] = [exception.Message] }); }
    catch (InvalidOperationException exception) { return Results.Conflict(new { title = exception.Message, status = 409 }); }
}).RequireAuthorization(policy => policy.RequireRole("admin"));
adminApi.MapDelete("/workstations/{workstationId:guid}/devices/{deviceId:guid}/grants/{entryId:guid}", async (Guid workstationId, Guid deviceId, Guid entryId, HttpContext context, UsbDlpRepository store, CancellationToken token) =>
    await store.RevokePairGrantAsync(workstationId, deviceId, entryId, context.User.Identity?.Name ?? "admin", token) ? Results.NoContent() : Results.NotFound()).RequireAuthorization(policy => policy.RequireRole("admin"));
adminApi.MapPost("/policies", async (HttpContext context, AddPolicyRequest request, UsbDlpRepository store, CancellationToken token) =>
{
    try
    {
        Guid id = await store.AddPolicyAsync(request, context.User.Identity?.Name ?? "admin", token);
        return Results.Created($"/api/v1/admin/policies/{id}", new { id });
    }
    catch (PolicyValidationException exception)
    {
        return Results.ValidationProblem(exception.Errors.GroupBy(error => error.Code).ToDictionary(group => group.Key, group => group.Select(error => error.Message).ToArray()));
    }
}).RequireAuthorization(policy => policy.RequireRole("admin"));
adminApi.MapDelete("/policies/{id:guid}", async (Guid id, HttpContext context, UsbDlpRepository store, CancellationToken token) =>
    await store.DeletePolicyAsync(id, context.User.Identity?.Name ?? "admin", token) ? Results.NoContent() : Results.NotFound()).RequireAuthorization(policy => policy.RequireRole("admin"));
adminApi.MapPost("/requests/{id:guid}/approve", async (Guid id, DecisionRequest request, HttpContext context, UsbDlpRepository store, CancellationToken token) =>
    await store.DecideRequestAsync(id, true, request, context.User.Identity?.Name ?? "admin", token) ? Results.Ok() : Results.NotFound()).RequireAuthorization(policy => policy.RequireRole("admin"));
adminApi.MapPost("/requests/{id:guid}/deny", async (Guid id, DecisionRequest request, HttpContext context, UsbDlpRepository store, CancellationToken token) =>
    await store.DecideRequestAsync(id, false, request, context.User.Identity?.Name ?? "admin", token) ? Results.Ok() : Results.NotFound()).RequireAuthorization(policy => policy.RequireRole("admin"));
adminApi.MapPost("/requests/{id:guid}/revoke", async (Guid id, HttpContext context, UsbDlpRepository store, CancellationToken token) =>
    await store.RevokeRequestApprovalAsync(id, context.User.Identity?.Name ?? "admin", token) ? Results.Ok() : Results.NotFound()).RequireAuthorization(policy => policy.RequireRole("admin"));

app.MapGet("/culture/{culture}", (string culture, string? returnUrl, HttpContext context) =>
{
    string selected = culture == "zh-CN" ? "zh-CN" : "en-US";
    context.Response.Cookies.Append(CookieRequestCultureProvider.DefaultCookieName,
        CookieRequestCultureProvider.MakeCookieValue(new RequestCulture(selected)),
        new CookieOptions { Expires = DateTimeOffset.UtcNow.AddYears(1), IsEssential = true, SameSite = SameSiteMode.Lax });
    return Results.LocalRedirect(returnUrl is not null && Uri.IsWellFormedUriString(returnUrl, UriKind.Relative) && returnUrl.StartsWith('/') ? returnUrl : "/");
}).AllowAnonymous();

app.MapRazorPages();
app.MapHealthChecks("/health/live").AllowAnonymous();
app.MapHealthChecks("/health/ready").AllowAnonymous();
app.Run();

static async Task<AuthenticatedAgent?> AuthenticateAsync(HttpContext context, UsbDlpRepository repository, CancellationToken cancellationToken)
{
    string authorization = context.Request.Headers.Authorization.ToString();
    return authorization.StartsWith("Bearer ", StringComparison.Ordinal) ? await repository.AuthenticateAsync(authorization[7..], cancellationToken) : null;
}

static bool ValidPage(int page, int pageSize) => page > 0 && pageSize is 25 or 50 or 100;
static bool ValidQuery(string? query) => query is null || query.Trim().Length <= 200;

public partial class Program;
