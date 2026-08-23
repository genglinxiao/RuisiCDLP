using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.Data.Sqlite;
using UsbDlp.Contracts;
using UsbDlp.Policy;
using MatchType = UsbDlp.Policy.MatchType;

namespace UsbDlp.Server.Data;

public sealed class UsbDlpRepository
{
    private static readonly TimeSpan RequestCooldown = TimeSpan.FromMinutes(10);
    private readonly string _connectionString;
    private readonly ILogger<UsbDlpRepository> _logger;

    public UsbDlpRepository(IConfiguration configuration, IWebHostEnvironment environment, ILogger<UsbDlpRepository> logger)
    {
        string path = configuration["UsbDlp:DatabasePath"] ??
            Path.Combine(environment.ContentRootPath, "App_Data", "usb-dlp.db");
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        _connectionString = new SqliteConnectionStringBuilder
        {
            DataSource = path,
            Mode = SqliteOpenMode.ReadWriteCreate,
            Cache = SqliteCacheMode.Shared,
            Pooling = false,
        }.ToString();
        _logger = logger;
    }

    public async Task InitializeAsync(string? bootstrapToken, CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await ExecuteAsync(connection, "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;", cancellationToken);
        await ExecuteAsync(connection, Schema, cancellationToken);
        await EnsureInventorySchemaAsync(connection, cancellationToken);

        await using SqliteCommand seed = connection.CreateCommand();
        seed.CommandText = """
            INSERT OR IGNORE INTO groups(id,name,mode,created_at_utc) VALUES($id,'Default','default_deny',$now);
            INSERT OR IGNORE INTO meta(key,value) VALUES('policy_version','1');
            """;
        seed.Parameters.AddWithValue("$id", DefaultGroupId.ToString());
        seed.Parameters.AddWithValue("$now", Utc(DateTimeOffset.UtcNow));
        await seed.ExecuteNonQueryAsync(cancellationToken);

        if (!string.IsNullOrWhiteSpace(bootstrapToken))
        {
            await EnsureEnrollmentTokenAsync(bootstrapToken, cancellationToken);
        }

        await BackfillInventoryAsync(cancellationToken);
    }

    public async Task EnsureEnrollmentTokenAsync(string token, CancellationToken cancellationToken)
    {
        byte[] hash = SHA256.HashData(Encoding.UTF8.GetBytes(token));
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using SqliteCommand command = connection.CreateCommand();
        command.CommandText = """
            INSERT OR IGNORE INTO enrollment_tokens(id,token_hash,created_at_utc,expires_at_utc,max_uses,use_count)
            VALUES($id,$hash,$now,$expires,1000,0);
            """;
        command.Parameters.AddWithValue("$id", Guid.NewGuid().ToString());
        command.Parameters.AddWithValue("$hash", hash);
        command.Parameters.AddWithValue("$now", Utc(DateTimeOffset.UtcNow));
        command.Parameters.AddWithValue("$expires", Utc(DateTimeOffset.UtcNow.AddYears(1)));
        await command.ExecuteNonQueryAsync(cancellationToken);
    }

    public async Task<(EnrollmentResponse Response, bool Created)?> EnrollAsync(
        EnrollmentRequest request,
        int pollSeconds,
        CancellationToken cancellationToken)
    {
        byte[] tokenHash = SHA256.HashData(Encoding.UTF8.GetBytes(request.EnrollmentToken));
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using SqliteTransaction transaction = (SqliteTransaction)await connection.BeginTransactionAsync(cancellationToken);

        await using SqliteCommand token = connection.CreateCommand();
        token.Transaction = transaction;
        token.CommandText = """
            SELECT id FROM enrollment_tokens
            WHERE token_hash=$hash AND revoked_at_utc IS NULL AND expires_at_utc>$now AND use_count<max_uses;
            """;
        token.Parameters.AddWithValue("$hash", tokenHash);
        token.Parameters.AddWithValue("$now", Utc(DateTimeOffset.UtcNow));
        string? tokenId = (string?)await token.ExecuteScalarAsync(cancellationToken);
        if (tokenId is null)
        {
            return null;
        }

        Guid workstationId;
        bool created;
        await using (SqliteCommand existing = connection.CreateCommand())
        {
            existing.Transaction = transaction;
            existing.CommandText = "SELECT id FROM workstations WHERE machine_sid=$sid AND revoked_at_utc IS NULL;";
            existing.Parameters.AddWithValue("$sid", request.MachineSid);
            string? existingId = (string?)await existing.ExecuteScalarAsync(cancellationToken);
            workstationId = existingId is null ? Guid.NewGuid() : Guid.Parse(existingId);
            created = existingId is null;
        }

        byte[] secretBytes = RandomNumberGenerator.GetBytes(32);
        string secret = Base64Url(secretBytes);
        byte[] salt = RandomNumberGenerator.GetBytes(32);
        byte[] keyHash = HashCredential(salt, secret);
        DateTimeOffset now = DateTimeOffset.UtcNow;

        await using (SqliteCommand upsert = connection.CreateCommand())
        {
            upsert.Transaction = transaction;
            upsert.CommandText = """
                INSERT INTO workstations(id,machine_sid,hostname,group_id,key_salt,key_hash,key_version,agent_version,
                    enrolled_at_utc,last_seen_utc,applied_policy_version,health)
                VALUES($id,$sid,$host,$group,$salt,$hash,1,$version,$now,$now,0,'healthy')
                ON CONFLICT(machine_sid) DO UPDATE SET hostname=$host,key_salt=$salt,key_hash=$hash,
                    key_version=key_version+1,agent_version=$version,last_seen_utc=$now,revoked_at_utc=NULL;
                """;
            upsert.Parameters.AddWithValue("$id", workstationId.ToString());
            upsert.Parameters.AddWithValue("$sid", request.MachineSid);
            upsert.Parameters.AddWithValue("$host", request.Hostname);
            upsert.Parameters.AddWithValue("$group", DefaultGroupId.ToString());
            upsert.Parameters.AddWithValue("$salt", salt);
            upsert.Parameters.AddWithValue("$hash", keyHash);
            upsert.Parameters.AddWithValue("$version", request.AgentVersion);
            upsert.Parameters.AddWithValue("$now", Utc(now));
            await upsert.ExecuteNonQueryAsync(cancellationToken);
        }

        await using (SqliteCommand useToken = connection.CreateCommand())
        {
            useToken.Transaction = transaction;
            useToken.CommandText = "UPDATE enrollment_tokens SET use_count=use_count+1 WHERE id=$id;";
            useToken.Parameters.AddWithValue("$id", tokenId);
            await useToken.ExecuteNonQueryAsync(cancellationToken);
        }

        await transaction.CommitAsync(cancellationToken);
        string credential = $"{workstationId:D}.{secret}";
        return (new(1, workstationId, credential, now, pollSeconds), created);
    }

    public async Task<AuthenticatedAgent?> AuthenticateAsync(string credential, CancellationToken cancellationToken)
    {
        int separator = credential.IndexOf('.', StringComparison.Ordinal);
        if (separator <= 0 || !Guid.TryParse(credential[..separator], out Guid workstationId))
        {
            return null;
        }

        string secret = credential[(separator + 1)..];
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using SqliteCommand command = connection.CreateCommand();
        command.CommandText = """
            SELECT group_id,hostname,key_salt,key_hash,applied_policy_version FROM workstations
            WHERE id=$id AND revoked_at_utc IS NULL;
            """;
        command.Parameters.AddWithValue("$id", workstationId.ToString());
        await using SqliteDataReader reader = await command.ExecuteReaderAsync(cancellationToken);
        if (!await reader.ReadAsync(cancellationToken))
        {
            return null;
        }

        byte[] actual = HashCredential((byte[])reader[2], secret);
        if (!CryptographicOperations.FixedTimeEquals(actual, (byte[])reader[3]))
        {
            return null;
        }

        return new(workstationId, Guid.Parse(reader.GetString(0)), reader.GetString(1), reader.GetInt64(4));
    }

    public async Task<CompiledPolicy> CompilePolicyAsync(AuthenticatedAgent agent, CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        long version = await PolicyVersionAsync(connection, cancellationToken);
        GroupMode mode;
        await using (SqliteCommand group = connection.CreateCommand())
        {
            group.CommandText = "SELECT mode FROM groups WHERE id=$id;";
            group.Parameters.AddWithValue("$id", agent.GroupId.ToString());
            mode = ParseGroupMode((string)(await group.ExecuteScalarAsync(cancellationToken) ?? "default_deny"));
        }

        List<DeviceEntry> entries = [];
        await using SqliteCommand command = connection.CreateCommand();
        command.CommandText = """
            SELECT id,match_type,vid,pid,serial,instance_id,device_class,display_name,action,mandatory,
                   scope_type,scope_id,expires_at_utc,note,device_install_ids_json
            FROM device_entries WHERE deleted_at_utc IS NULL AND
              (scope_type='global' OR (scope_type='group' AND scope_id=$group) OR
               (scope_type='workstation' AND scope_id=$workstation));
            """;
        command.Parameters.AddWithValue("$group", agent.GroupId.ToString());
        command.Parameters.AddWithValue("$workstation", agent.WorkstationId.ToString());
        await using SqliteDataReader reader = await command.ExecuteReaderAsync(cancellationToken);
        while (await reader.ReadAsync(cancellationToken))
        {
            entries.Add(ReadEntry(reader));
        }

        return new(1, version, DateTimeOffset.UtcNow, agent.WorkstationId, agent.GroupId, mode, entries);
    }

    public async Task<EventBatchResponse> AddEventsAsync(
        AuthenticatedAgent agent,
        EventBatchRequest batch,
        CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using SqliteTransaction transaction = (SqliteTransaction)await connection.BeginTransactionAsync(cancellationToken);
        List<Guid> accepted = [];
        List<Guid> duplicates = [];
        foreach (AgentEvent item in batch.Events)
        {
            await using SqliteCommand command = connection.CreateCommand();
            command.Transaction = transaction;
            command.CommandText = """
                INSERT OR IGNORE INTO events(id,workstation_id,client_event_id,type,occurred_at_utc,received_at_utc,
                    hostname,user_sid,username,device_json,policy_version,decision,matched_entry_id,reason_code,agent_version,details_json)
                VALUES($id,$ws,$client,$type,$occurred,$received,$host,$sid,$user,$device,$policy,$decision,$entry,$reason,$agent,$details);
                """;
            command.Parameters.AddWithValue("$id", Guid.NewGuid().ToString());
            command.Parameters.AddWithValue("$ws", agent.WorkstationId.ToString());
            command.Parameters.AddWithValue("$client", item.ClientEventId.ToString());
            command.Parameters.AddWithValue("$type", item.Type);
            command.Parameters.AddWithValue("$occurred", Utc(item.OccurredAtUtc));
            command.Parameters.AddWithValue("$received", Utc(DateTimeOffset.UtcNow));
            command.Parameters.AddWithValue("$host", item.Hostname);
            command.Parameters.AddWithValue("$sid", Db(item.UserSid));
            command.Parameters.AddWithValue("$user", Db(item.Username));
            command.Parameters.AddWithValue("$device", Db(item.Device is null ? null : JsonSerializer.Serialize(item.Device, PolicyJson.Options)));
            command.Parameters.AddWithValue("$policy", item.PolicyVersion);
            command.Parameters.AddWithValue("$decision", Db(item.Decision?.ToString().ToLowerInvariant()));
            command.Parameters.AddWithValue("$entry", Db(item.MatchedEntryId?.ToString()));
            command.Parameters.AddWithValue("$reason", Db(item.ReasonCode));
            command.Parameters.AddWithValue("$agent", item.AgentVersion);
            command.Parameters.AddWithValue("$details", item.Details.GetRawText());
            int changed = await command.ExecuteNonQueryAsync(cancellationToken);
            if (changed == 1)
            {
                accepted.Add(item.ClientEventId);
                if (item.Device is not null)
                {
                    (Guid deviceId, Guid pairId) = await UpsertInventoryAsync(
                        connection, transaction, agent.WorkstationId, item.Device, item.OccurredAtUtc,
                        item.Type, item.Decision?.ToString().ToLowerInvariant(), item.ReasonCode,
                        item.PolicyVersion, cancellationToken);
                    await using SqliteCommand link = connection.CreateCommand();
                    link.Transaction = transaction;
                    link.CommandText = "UPDATE events SET usb_device_id=$device,workstation_device_id=$pair WHERE workstation_id=$ws AND client_event_id=$client;";
                    link.Parameters.AddWithValue("$device", deviceId.ToString());
                    link.Parameters.AddWithValue("$pair", pairId.ToString());
                    link.Parameters.AddWithValue("$ws", agent.WorkstationId.ToString());
                    link.Parameters.AddWithValue("$client", item.ClientEventId.ToString());
                    await link.ExecuteNonQueryAsync(cancellationToken);
                }
            }
            else
            {
                duplicates.Add(item.ClientEventId);
            }
        }

        long version = await PolicyVersionAsync(connection, transaction, cancellationToken);
        await transaction.CommitAsync(cancellationToken);
        return new(1, accepted, duplicates, version != agent.AppliedPolicyVersion, version);
    }

    public async Task<(AccessRequestResponse Response, bool Created, Guid? Existing)> AddRequestAsync(
        AuthenticatedAgent agent,
        AccessRequestCreate request,
        CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using (SqliteCommand replay = connection.CreateCommand())
        {
            replay.CommandText = "SELECT id,status,created_at_utc FROM access_requests WHERE workstation_id=$ws AND client_request_id=$client;";
            replay.Parameters.AddWithValue("$ws", agent.WorkstationId.ToString());
            replay.Parameters.AddWithValue("$client", request.ClientRequestId.ToString());
            await using SqliteDataReader reader = await replay.ExecuteReaderAsync(cancellationToken);
            if (await reader.ReadAsync(cancellationToken))
            {
                return (new(1, Guid.Parse(reader.GetString(0)), reader.GetString(1), ParseUtc(reader.GetString(2))), false, null);
            }
        }

        await using (SqliteCommand recent = connection.CreateCommand())
        {
            recent.CommandText = """
                SELECT id,created_at_utc FROM access_requests WHERE workstation_id=$ws AND user_sid=$sid
                  AND instance_id=$instance AND created_at_utc >= $cutoff
                ORDER BY created_at_utc DESC LIMIT 1;
                """;
            DateTimeOffset cooldownNow = DateTimeOffset.UtcNow;
            recent.Parameters.AddWithValue("$ws", agent.WorkstationId.ToString());
            recent.Parameters.AddWithValue("$sid", request.UserSid);
            recent.Parameters.AddWithValue("$instance", request.Device.InstanceId);
            recent.Parameters.AddWithValue("$cutoff", Utc(cooldownNow.Subtract(RequestCooldown)));
            await using SqliteDataReader reader = await recent.ExecuteReaderAsync(cancellationToken);
            if (await reader.ReadAsync(cancellationToken))
            {
                Guid id = Guid.Parse(reader.GetString(0));
                DateTimeOffset createdAt = ParseUtc(reader.GetString(1));
                return (new(1, id, "recent", createdAt, createdAt.Add(RequestCooldown)), false, id);
            }
        }

        Guid requestId = Guid.NewGuid();
        DateTimeOffset now = DateTimeOffset.UtcNow;
        await using SqliteTransaction transaction = (SqliteTransaction)await connection.BeginTransactionAsync(cancellationToken);
        (Guid deviceId, Guid pairId) = await UpsertInventoryAsync(
            connection, transaction, agent.WorkstationId, request.Device, request.OccurredAtUtc,
            "access_request", null, null, agent.AppliedPolicyVersion, cancellationToken);
        await using SqliteCommand insert = connection.CreateCommand();
        insert.Transaction = transaction;
        insert.CommandText = """
            INSERT INTO access_requests(id,client_request_id,workstation_id,user_sid,username,device_json,instance_id,
                requested_duration_days,justification,status,created_at_utc,usb_device_id,workstation_device_id)
            VALUES($id,$client,$ws,$sid,$user,$device,$instance,$days,$justification,'pending',$now,$deviceId,$pairId);
            """;
        insert.Parameters.AddWithValue("$id", requestId.ToString());
        insert.Parameters.AddWithValue("$client", request.ClientRequestId.ToString());
        insert.Parameters.AddWithValue("$ws", agent.WorkstationId.ToString());
        insert.Parameters.AddWithValue("$sid", request.UserSid);
        insert.Parameters.AddWithValue("$user", request.Username);
        insert.Parameters.AddWithValue("$device", JsonSerializer.Serialize(request.Device, PolicyJson.Options));
        insert.Parameters.AddWithValue("$instance", request.Device.InstanceId);
        insert.Parameters.AddWithValue("$days", request.RequestedDurationDays);
        insert.Parameters.AddWithValue("$justification", request.Justification.Trim());
        insert.Parameters.AddWithValue("$now", Utc(now));
        insert.Parameters.AddWithValue("$deviceId", deviceId.ToString());
        insert.Parameters.AddWithValue("$pairId", pairId.ToString());
        await insert.ExecuteNonQueryAsync(cancellationToken);
        await transaction.CommitAsync(cancellationToken);
        return (new(1, requestId, "pending", now, now.Add(RequestCooldown)), true, null);
    }

    public async Task<long> HeartbeatAsync(
        AuthenticatedAgent agent,
        HeartbeatRequest heartbeat,
        CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using SqliteCommand update = connection.CreateCommand();
        update.CommandText = """
            UPDATE workstations SET agent_version=$agent,last_seen_utc=$now,applied_policy_version=$policy,
              health=$health,last_error=$error WHERE id=$id;
            """;
        update.Parameters.AddWithValue("$agent", heartbeat.AgentVersion);
        update.Parameters.AddWithValue("$now", Utc(DateTimeOffset.UtcNow));
        update.Parameters.AddWithValue("$policy", heartbeat.AppliedPolicyVersion);
        update.Parameters.AddWithValue("$health", Snake(heartbeat.Health.ToString()));
        update.Parameters.AddWithValue("$error", Db(heartbeat.LastEnforcementErrorCode));
        update.Parameters.AddWithValue("$id", agent.WorkstationId.ToString());
        await update.ExecuteNonQueryAsync(cancellationToken);
        return await PolicyVersionAsync(connection, cancellationToken);
    }

    public async Task<DashboardModel> DashboardAsync(CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        long version = await PolicyVersionAsync(connection, cancellationToken);
        List<WorkstationRow> workstations = await WorkstationsAsync(connection, cancellationToken);
        List<PolicyRow> policies = await PoliciesAsync(connection, cancellationToken);
        List<RequestRow> requests = await RequestsAsync(connection, cancellationToken);
        List<EventRow> events = await EventsAsync(connection, cancellationToken);
        List<AuditRow> audit = await AuditAsync(connection, cancellationToken);
        DateTimeOffset today = new(DateTime.UtcNow.Date, TimeSpan.Zero);
        return new(
            version,
            workstations.Count,
            workstations.Count(item => item.LastSeenUtc >= DateTimeOffset.UtcNow.AddMinutes(-10)),
            events.Count(item => item.Type == "blocked" && item.OccurredAtUtc >= today),
            requests.Count(item => item.Status == "pending"),
            workstations,
            policies,
            requests,
            events,
            audit);
    }

    public async Task<PagedResult<DeviceInventoryRow>> SearchDevicesAsync(string? query, int page, int pageSize, string sort, CancellationToken cancellationToken)
    {
        string normalized = NormalizeSearch(query ?? string.Empty);
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        string predicate = BuildTokenPredicate(normalized, "UPPER(d.id||' '||d.search_text||' '||COALESCE(d.admin_description,'')||' '||COALESCE((SELECT group_concat(wd.instance_id,' ') FROM workstation_devices wd WHERE wd.usb_device_id=d.id),''))", out string[] terms);
        int count = await CountAsync(connection, $"SELECT COUNT(*) FROM usb_devices d WHERE {predicate};", terms, cancellationToken);
        string order = sort switch
        {
            "name" => "d.friendly_name COLLATE NOCASE,d.id",
            "manufacturer" => "COALESCE(d.manufacturer,'') COLLATE NOCASE,d.friendly_name COLLATE NOCASE,d.id",
            "model" => "COALESCE(d.model,'') COLLATE NOCASE,d.friendly_name COLLATE NOCASE,d.id",
            _ => "d.last_seen_utc DESC,d.id",
        };
        List<DeviceInventoryRow> items = [];
        await using SqliteCommand command = connection.CreateCommand();
        command.CommandText = $"""
            SELECT d.id,d.identity_confidence,d.vid,d.pid,d.serial,d.friendly_name,d.manufacturer,d.model,
                   COALESCE(d.admin_description,d.observed_description),d.first_seen_utc,d.last_seen_utc,
                   (SELECT COUNT(*) FROM workstation_devices wd WHERE wd.usb_device_id=d.id)
            FROM usb_devices d WHERE {predicate} ORDER BY {order} LIMIT $limit OFFSET $offset;
            """;
        AddSearchParameters(command, terms);
        command.Parameters.AddWithValue("$limit", pageSize);
        command.Parameters.AddWithValue("$offset", (page - 1) * pageSize);
        await using SqliteDataReader reader = await command.ExecuteReaderAsync(cancellationToken);
        while (await reader.ReadAsync(cancellationToken)) items.Add(ReadDeviceRow(reader));
        return new(items, page, pageSize, count, normalized);
    }

    public async Task<PagedResult<WorkstationInventoryRow>> SearchWorkstationsAsync(string? query, int page, int pageSize, string sort, CancellationToken cancellationToken)
    {
        string normalized = NormalizeSearch(query ?? string.Empty);
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        string predicate = BuildTokenPredicate(normalized, "UPPER(w.id||' '||w.hostname||' '||w.machine_sid||' '||COALESCE(w.description,''))", out string[] terms);
        int count = await CountAsync(connection, $"SELECT COUNT(*) FROM workstations w WHERE w.revoked_at_utc IS NULL AND {predicate};", terms, cancellationToken);
        string order = sort == "hostname" ? "w.hostname COLLATE NOCASE,w.id" : "COALESCE(w.last_seen_utc,'') DESC,w.id";
        List<WorkstationInventoryRow> items = [];
        await using SqliteCommand command = connection.CreateCommand();
        command.CommandText = $"""
            SELECT w.id,w.hostname,w.machine_sid,g.name,w.description,w.agent_version,w.last_seen_utc,w.health,
                   (SELECT COUNT(*) FROM workstation_devices wd WHERE wd.workstation_id=w.id)
            FROM workstations w JOIN groups g ON g.id=w.group_id
            WHERE w.revoked_at_utc IS NULL AND {predicate} ORDER BY {order} LIMIT $limit OFFSET $offset;
            """;
        AddSearchParameters(command, terms);
        command.Parameters.AddWithValue("$limit", pageSize);
        command.Parameters.AddWithValue("$offset", (page - 1) * pageSize);
        await using SqliteDataReader reader = await command.ExecuteReaderAsync(cancellationToken);
        while (await reader.ReadAsync(cancellationToken)) items.Add(ReadWorkstationInventoryRow(reader));
        return new(items, page, pageSize, count, normalized);
    }

    public async Task<InventorySearchResult> SearchInventoryAsync(string? query, string kind, int page, int pageSize, CancellationToken cancellationToken)
    {
        PagedResult<DeviceInventoryRow> devices = kind == "workstation"
            ? new([], page, pageSize, 0, NormalizeSearch(query ?? string.Empty))
            : await SearchDevicesAsync(query, page, pageSize, "lastSeen", cancellationToken);
        PagedResult<WorkstationInventoryRow> workstations = kind == "device"
            ? new([], page, pageSize, 0, NormalizeSearch(query ?? string.Empty))
            : await SearchWorkstationsAsync(query, page, pageSize, "lastSeen", cancellationToken);
        return new(devices.Items, workstations.Items, page, pageSize, devices.Query);
    }

    public async Task<DeviceDetail?> DeviceDetailAsync(Guid deviceId, CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        DeviceInventoryRow? device = null;
        await using (SqliteCommand command = connection.CreateCommand())
        {
            command.CommandText = """
                SELECT d.id,d.identity_confidence,d.vid,d.pid,d.serial,d.friendly_name,d.manufacturer,d.model,
                       COALESCE(d.admin_description,d.observed_description),d.first_seen_utc,d.last_seen_utc,
                       (SELECT COUNT(*) FROM workstation_devices wd WHERE wd.usb_device_id=d.id)
                FROM usb_devices d WHERE d.id=$id;
                """;
            command.Parameters.AddWithValue("$id", deviceId.ToString());
            await using SqliteDataReader reader = await command.ExecuteReaderAsync(cancellationToken);
            if (await reader.ReadAsync(cancellationToken)) device = ReadDeviceRow(reader);
        }
        if (device is null) return null;
        List<Guid> workstations = await RelatedIdsAsync(connection, "SELECT workstation_id FROM workstation_devices WHERE usb_device_id=$id ORDER BY last_seen_utc DESC;", deviceId, cancellationToken);
        List<WorkstationDeviceRow> pairs = [];
        foreach (Guid workstationId in workstations)
        {
            WorkstationDeviceRow? pair = await PairDetailAsync(workstationId, deviceId, cancellationToken);
            if (pair is not null) pairs.Add(pair);
        }
        return new(device, pairs);
    }

    public async Task<WorkstationDetail?> WorkstationDetailAsync(Guid workstationId, CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        WorkstationInventoryRow? workstation = null;
        await using (SqliteCommand command = connection.CreateCommand())
        {
            command.CommandText = """
                SELECT w.id,w.hostname,w.machine_sid,g.name,w.description,w.agent_version,w.last_seen_utc,w.health,
                       (SELECT COUNT(*) FROM workstation_devices wd WHERE wd.workstation_id=w.id)
                FROM workstations w JOIN groups g ON g.id=w.group_id WHERE w.id=$id AND w.revoked_at_utc IS NULL;
                """;
            command.Parameters.AddWithValue("$id", workstationId.ToString());
            await using SqliteDataReader reader = await command.ExecuteReaderAsync(cancellationToken);
            if (await reader.ReadAsync(cancellationToken)) workstation = ReadWorkstationInventoryRow(reader);
        }
        if (workstation is null) return null;
        List<Guid> devices = await RelatedIdsAsync(connection, "SELECT usb_device_id FROM workstation_devices WHERE workstation_id=$id ORDER BY last_seen_utc DESC;", workstationId, cancellationToken);
        List<WorkstationDeviceRow> pairs = [];
        foreach (Guid deviceId in devices)
        {
            WorkstationDeviceRow? pair = await PairDetailAsync(workstationId, deviceId, cancellationToken);
            if (pair is not null) pairs.Add(pair);
        }
        return new(workstation, pairs);
    }

    public async Task<WorkstationDeviceRow?> PairDetailAsync(Guid workstationId, Guid deviceId, CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        string hostname;
        Guid groupId;
        DateTimeOffset? workstationLastSeen;
        long appliedVersion;
        string deviceName;
        string instanceId;
        string snapshotJson;
        string? lastDecision;
        string? lastReason;
        long lastPolicy;
        DateTimeOffset? lastObserved;
        string? lastEvent;
        await using (SqliteCommand command = connection.CreateCommand())
        {
            command.CommandText = """
                SELECT w.hostname,w.group_id,w.last_seen_utc,w.applied_policy_version,d.friendly_name,
                       wd.instance_id,wd.snapshot_json,wd.last_decision,wd.last_reason_code,wd.last_policy_version,
                       wd.last_observed_at_utc,wd.last_event_type
                FROM workstation_devices wd JOIN workstations w ON w.id=wd.workstation_id
                JOIN usb_devices d ON d.id=wd.usb_device_id
                WHERE wd.workstation_id=$ws AND wd.usb_device_id=$device;
                """;
            command.Parameters.AddWithValue("$ws", workstationId.ToString());
            command.Parameters.AddWithValue("$device", deviceId.ToString());
            await using SqliteDataReader reader = await command.ExecuteReaderAsync(cancellationToken);
            if (!await reader.ReadAsync(cancellationToken)) return null;
            hostname = reader.GetString(0); groupId = Guid.Parse(reader.GetString(1));
            workstationLastSeen = reader.IsDBNull(2) ? null : ParseUtc(reader.GetString(2));
            appliedVersion = reader.GetInt64(3); deviceName = reader.GetString(4); instanceId = reader.GetString(5);
            snapshotJson = reader.GetString(6); lastDecision = NullableString(reader, 7); lastReason = NullableString(reader, 8);
            lastPolicy = reader.GetInt64(9); lastObserved = reader.IsDBNull(10) ? null : ParseUtc(reader.GetString(10));
            lastEvent = NullableString(reader, 11);
        }

        DeviceSnapshot snapshot = JsonSerializer.Deserialize<DeviceSnapshot>(snapshotJson, PolicyJson.Options)!;
        CompiledPolicy policy = await CompilePolicyAsync(new(workstationId, groupId, hostname, appliedVersion), cancellationToken);
        DeviceInfo info = SnapshotToDeviceInfo(snapshot);
        PolicyEvaluation evaluation = PolicyEvaluator.Evaluate(info, policy, DateTimeOffset.UtcNow);
        DeviceEntry? matched = policy.Entries.FirstOrDefault(entry => entry.Id == evaluation.MatchedEntryId);
        string freshness = workstationLastSeen is null || workstationLastSeen < DateTimeOffset.UtcNow.AddMinutes(-10)
            ? "offline"
            : lastObserved is null || lastObserved < DateTimeOffset.UtcNow.AddHours(-24) ? "stale" : "current";
        string observed = lastObserved is null ? "unknown"
            : lastPolicy < policy.PolicyVersion ? "pending_change"
            : lastDecision is "allow" or "block" ? lastDecision + "ed" : "unknown";
        List<RequestRow> requests = await RequestsForPairAsync(connection, workstationId, instanceId, cancellationToken);
        string? qualification = requests.FirstOrDefault()?.Status switch
        {
            "pending" => "pending_request",
            "revoked" => "approval_revoked",
            "approved" when matched?.ExpiresAtUtc <= DateTimeOffset.UtcNow => "approval_expired",
            _ => null,
        };
        string policySource = matched is null ? evaluation.ReasonCode : $"{Snake(matched.ScopeType.ToString())}:{matched.DisplayName}";
        bool canRevoke = matched is { Action: EntryAction.Allow, ScopeType: ScopeType.Workstation } &&
            !string.Equals(matched.Note, "Approved request", StringComparison.Ordinal);
        return new(workstationId, deviceId, hostname, deviceName, instanceId,
            evaluation.Decision.ToString().ToLowerInvariant(), policySource, canRevoke, evaluation.MatchedEntryId, evaluation.ReasonCode,
            matched?.ExpiresAtUtc, policy.PolicyVersion, qualification, observed, lastObserved, freshness,
            lastEvent, requests);
    }

    public async Task<GrantResponse?> AddPairGrantAsync(Guid workstationId, Guid deviceId, GrantRequest request, string actor, CancellationToken cancellationToken)
    {
        if (request.Reason.Trim().Length is < 10 or > 1000 || request.ExpiresAtUtc is { } expiry &&
            (expiry <= DateTimeOffset.UtcNow || expiry > DateTimeOffset.UtcNow.AddDays(365)))
            throw new ArgumentException("Grant reason or expiry is invalid.");
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        DeviceSnapshot? snapshot = await PairSnapshotAsync(connection, workstationId, deviceId, cancellationToken);
        if (snapshot is null) return null;
        Guid entryId = Guid.NewGuid();
        DeviceEntry entry = string.IsNullOrWhiteSpace(snapshot.Serial)
            ? new(entryId, MatchType.InstanceId, null, null, null, snapshot.InstanceId, null, snapshot.FriendlyName,
                EntryAction.Allow, false, ScopeType.Workstation, workstationId, request.ExpiresAtUtc, request.Reason.Trim(), snapshot.DeviceInstallIds)
            : new(entryId, MatchType.VidPidSerial, snapshot.Vid, snapshot.Pid, snapshot.Serial, null, null, snapshot.FriendlyName,
                EntryAction.Allow, false, ScopeType.Workstation, workstationId, request.ExpiresAtUtc, request.Reason.Trim(), snapshot.DeviceInstallIds);
        await using SqliteTransaction transaction = (SqliteTransaction)await connection.BeginTransactionAsync(cancellationToken);
        await using SqliteCommand duplicate = connection.CreateCommand();
        duplicate.Transaction = transaction;
        duplicate.CommandText = """
            SELECT COUNT(*) FROM device_entries WHERE deleted_at_utc IS NULL AND action='allow' AND
            scope_type='workstation' AND scope_id=$ws AND
            ((match_type='instance_id' AND instance_id=$instance) OR
             (match_type='vid_pid_serial' AND vid=$vid AND pid=$pid AND serial=$serial)) AND
            (expires_at_utc IS NULL OR expires_at_utc>$now);
            """;
        duplicate.Parameters.AddWithValue("$ws", workstationId.ToString());
        duplicate.Parameters.AddWithValue("$instance", Db(entry.InstanceId)); duplicate.Parameters.AddWithValue("$vid", Db(entry.Vid));
        duplicate.Parameters.AddWithValue("$pid", Db(entry.Pid)); duplicate.Parameters.AddWithValue("$serial", Db(entry.Serial));
        duplicate.Parameters.AddWithValue("$now", Utc(DateTimeOffset.UtcNow));
        if (Convert.ToInt32(await duplicate.ExecuteScalarAsync(cancellationToken), CultureInfo.InvariantCulture) > 0)
            throw new InvalidOperationException("An equivalent active grant already exists.");
        await using SqliteCommand add = connection.CreateCommand();
        add.Transaction = transaction;
        add.CommandText = """
            INSERT INTO device_entries(id,match_type,vid,pid,serial,instance_id,device_class,display_name,action,mandatory,
                scope_type,scope_id,expires_at_utc,note,device_install_ids_json,created_at_utc,updated_at_utc)
            VALUES($id,$match,$vid,$pid,$serial,$instance,$class,$name,$action,$mandatory,$scope,$scopeId,$expires,$note,$install,$now,$now);
            """;
        AddEntryParameters(add, entry);
        await add.ExecuteNonQueryAsync(cancellationToken);
        await BumpPolicyAsync(connection, transaction, actor, "pair_grant_added", "WorkstationDevice", $"{workstationId}:{deviceId}", cancellationToken);
        await transaction.CommitAsync(cancellationToken);
        long version = await PolicyVersionAsync(connection, cancellationToken);
        WorkstationDeviceRow pair = (await PairDetailAsync(workstationId, deviceId, cancellationToken))!;
        return new(entryId, version, pair);
    }

    public async Task<bool> RevokePairGrantAsync(Guid workstationId, Guid deviceId, Guid entryId, string actor, CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        DeviceSnapshot? snapshot = await PairSnapshotAsync(connection, workstationId, deviceId, cancellationToken);
        if (snapshot is null) return false;
        await using SqliteTransaction transaction = (SqliteTransaction)await connection.BeginTransactionAsync(cancellationToken);
        await using SqliteCommand revoke = connection.CreateCommand();
        revoke.Transaction = transaction;
        revoke.CommandText = """
            UPDATE device_entries SET deleted_at_utc=$now,updated_at_utc=$now
            WHERE id=$id AND scope_type='workstation' AND scope_id=$ws AND action='allow' AND deleted_at_utc IS NULL AND
              ((match_type='instance_id' AND instance_id=$instance) OR
               (match_type='vid_pid_serial' AND vid=$vid AND pid=$pid AND serial=$serial));
            """;
        revoke.Parameters.AddWithValue("$now", Utc(DateTimeOffset.UtcNow)); revoke.Parameters.AddWithValue("$id", entryId.ToString());
        revoke.Parameters.AddWithValue("$ws", workstationId.ToString()); revoke.Parameters.AddWithValue("$instance", snapshot.InstanceId);
        revoke.Parameters.AddWithValue("$vid", snapshot.Vid); revoke.Parameters.AddWithValue("$pid", snapshot.Pid);
        revoke.Parameters.AddWithValue("$serial", Db(snapshot.Serial));
        if (await revoke.ExecuteNonQueryAsync(cancellationToken) == 0) return false;
        await BumpPolicyAsync(connection, transaction, actor, "pair_grant_revoked", "WorkstationDevice", $"{workstationId}:{deviceId}", cancellationToken);
        await transaction.CommitAsync(cancellationToken);
        return true;
    }

    public async Task<bool> UpdateDescriptionAsync(string kind, Guid id, string? description, string actor, CancellationToken cancellationToken)
    {
        string? value = TrimOrNull(description);
        if (value?.Length > 500) throw new ArgumentException("Description cannot exceed 500 characters.");
        string table = kind == "device" ? "usb_devices" : kind == "workstation" ? "workstations" : throw new ArgumentException("Unknown inventory kind.");
        string column = kind == "device" ? "admin_description" : "description";
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using SqliteTransaction transaction = (SqliteTransaction)await connection.BeginTransactionAsync(cancellationToken);
        await using SqliteCommand update = connection.CreateCommand(); update.Transaction = transaction;
        update.CommandText = $"UPDATE {table} SET {column}=$description{(kind == "device" ? ",updated_at_utc=$now" : string.Empty)} WHERE id=$id;";
        update.Parameters.AddWithValue("$description", Db(value)); update.Parameters.AddWithValue("$id", id.ToString());
        update.Parameters.AddWithValue("$now", Utc(DateTimeOffset.UtcNow));
        if (await update.ExecuteNonQueryAsync(cancellationToken) == 0) return false;
        await using SqliteCommand audit = connection.CreateCommand(); audit.Transaction = transaction;
        audit.CommandText = "INSERT INTO audit_logs(id,actor,action,target_type,target_id,occurred_at_utc) VALUES($audit,$actor,'inventory_description_updated',$type,$target,$now);";
        audit.Parameters.AddWithValue("$audit", Guid.NewGuid().ToString()); audit.Parameters.AddWithValue("$actor", actor);
        audit.Parameters.AddWithValue("$type", kind == "device" ? "UsbDevice" : "Workstation"); audit.Parameters.AddWithValue("$target", id.ToString());
        audit.Parameters.AddWithValue("$now", Utc(DateTimeOffset.UtcNow)); await audit.ExecuteNonQueryAsync(cancellationToken);
        await transaction.CommitAsync(cancellationToken); return true;
    }

    public async Task<Guid> AddPolicyAsync(AddPolicyRequest request, string actor, CancellationToken cancellationToken)
    {
        Guid id = Guid.NewGuid();
        DeviceEntry entry = new(
            id, request.MatchType, request.Vid, request.Pid, request.Serial, request.InstanceId,
            request.DeviceClass, request.DisplayName, request.Action, request.Mandatory,
            request.ScopeType, request.ScopeId, request.ExpiresAtUtc, null, []);
        CompiledPolicy validationPolicy = new(1, 1, DateTimeOffset.UtcNow, request.ScopeType == ScopeType.Workstation ? request.ScopeId ?? Guid.Empty : Guid.NewGuid(), request.ScopeType == ScopeType.Group ? request.ScopeId ?? Guid.Empty : Guid.NewGuid(), GroupMode.DefaultDeny, [entry]);
        CompiledPolicyValidator.ValidateAndThrow(validationPolicy);

        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using SqliteTransaction transaction = (SqliteTransaction)await connection.BeginTransactionAsync(cancellationToken);
        await using SqliteCommand command = connection.CreateCommand();
        command.Transaction = transaction;
        command.CommandText = """
            INSERT INTO device_entries(id,match_type,vid,pid,serial,instance_id,device_class,display_name,action,mandatory,
                scope_type,scope_id,expires_at_utc,note,device_install_ids_json,created_at_utc,updated_at_utc)
            VALUES($id,$match,$vid,$pid,$serial,$instance,$class,$name,$action,$mandatory,$scope,$scopeId,$expires,NULL,'[]',$now,$now);
            """;
        AddEntryParameters(command, entry);
        await command.ExecuteNonQueryAsync(cancellationToken);
        await BumpPolicyAsync(connection, transaction, actor, "policy_added", "DeviceEntry", id.ToString(), cancellationToken);
        await transaction.CommitAsync(cancellationToken);
        return id;
    }

    public async Task<bool> DeletePolicyAsync(Guid id, string actor, CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using SqliteTransaction transaction = (SqliteTransaction)await connection.BeginTransactionAsync(cancellationToken);
        await using SqliteCommand command = connection.CreateCommand();
        command.Transaction = transaction;
        command.CommandText = "UPDATE device_entries SET deleted_at_utc=$now,updated_at_utc=$now WHERE id=$id AND deleted_at_utc IS NULL;";
        command.Parameters.AddWithValue("$id", id.ToString());
        command.Parameters.AddWithValue("$now", Utc(DateTimeOffset.UtcNow));
        if (await command.ExecuteNonQueryAsync(cancellationToken) == 0)
            return false;
        await BumpPolicyAsync(connection, transaction, actor, "policy_deleted", "DeviceEntry", id.ToString(), cancellationToken);
        await transaction.CommitAsync(cancellationToken);
        return true;
    }

    public async Task<bool> DecideRequestAsync(Guid requestId, bool approve, DecisionRequest decision, string actor, CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using SqliteTransaction transaction = (SqliteTransaction)await connection.BeginTransactionAsync(cancellationToken);
        await using SqliteCommand select = connection.CreateCommand();
        select.Transaction = transaction;
        select.CommandText = "SELECT workstation_id,device_json,status FROM access_requests WHERE id=$id;";
        select.Parameters.AddWithValue("$id", requestId.ToString());
        await using SqliteDataReader reader = await select.ExecuteReaderAsync(cancellationToken);
        if (!await reader.ReadAsync(cancellationToken) || reader.GetString(2) != "pending")
        {
            return false;
        }

        Guid workstationId = Guid.Parse(reader.GetString(0));
        DeviceSnapshot device = JsonSerializer.Deserialize<DeviceSnapshot>(reader.GetString(1), PolicyJson.Options)!;
        await reader.DisposeAsync();
        Guid? resultingEntry = null;
        if (approve)
        {
            resultingEntry = Guid.NewGuid();
            DeviceEntry entry = new(
                resultingEntry.Value, MatchType.InstanceId, null, null, null, device.InstanceId, null,
                device.FriendlyName, EntryAction.Allow, false, ScopeType.Workstation, workstationId,
                DateTimeOffset.UtcNow.AddDays(decision.DurationDays), "Approved request", device.DeviceInstallIds);
            await using SqliteCommand add = connection.CreateCommand();
            add.Transaction = transaction;
            add.CommandText = """
                INSERT INTO device_entries(id,match_type,vid,pid,serial,instance_id,device_class,display_name,action,mandatory,
                    scope_type,scope_id,expires_at_utc,note,device_install_ids_json,created_at_utc,updated_at_utc)
                VALUES($id,$match,$vid,$pid,$serial,$instance,$class,$name,$action,$mandatory,$scope,$scopeId,$expires,$note,$install,$now,$now);
                """;
            AddEntryParameters(add, entry);
            await add.ExecuteNonQueryAsync(cancellationToken);
        }

        await using SqliteCommand update = connection.CreateCommand();
        update.Transaction = transaction;
        update.CommandText = """
            UPDATE access_requests SET status=$status,decision_reason=$reason,decided_by=$actor,decided_at_utc=$now,
                resulting_entry_id=$entry WHERE id=$id;
            """;
        update.Parameters.AddWithValue("$status", approve ? "approved" : "denied");
        update.Parameters.AddWithValue("$reason", decision.Reason ?? string.Empty);
        update.Parameters.AddWithValue("$actor", actor);
        update.Parameters.AddWithValue("$now", Utc(DateTimeOffset.UtcNow));
        update.Parameters.AddWithValue("$entry", Db(resultingEntry?.ToString()));
        update.Parameters.AddWithValue("$id", requestId.ToString());
        await update.ExecuteNonQueryAsync(cancellationToken);
        await BumpPolicyAsync(connection, transaction, actor, approve ? "request_approved" : "request_denied", "AccessRequest", requestId.ToString(), cancellationToken);
        await transaction.CommitAsync(cancellationToken);
        return true;
    }

    public async Task<bool> RevokeRequestApprovalAsync(Guid requestId, string actor, CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using SqliteTransaction transaction = (SqliteTransaction)await connection.BeginTransactionAsync(cancellationToken);
        await using SqliteCommand select = connection.CreateCommand();
        select.Transaction = transaction;
        select.CommandText = "SELECT resulting_entry_id,status FROM access_requests WHERE id=$id;";
        select.Parameters.AddWithValue("$id", requestId.ToString());
        await using SqliteDataReader reader = await select.ExecuteReaderAsync(cancellationToken);
        if (!await reader.ReadAsync(cancellationToken) || reader.GetString(1) != "approved" || reader.IsDBNull(0))
            return false;
        Guid entryId = Guid.Parse(reader.GetString(0));
        await reader.DisposeAsync();

        string now = Utc(DateTimeOffset.UtcNow);
        await using SqliteCommand revoke = connection.CreateCommand();
        revoke.Transaction = transaction;
        revoke.CommandText = """
            UPDATE device_entries SET deleted_at_utc=$now,updated_at_utc=$now WHERE id=$entry AND deleted_at_utc IS NULL;
            UPDATE access_requests SET status='revoked',decision_reason='Revoked by administrator',decided_by=$actor,decided_at_utc=$now WHERE id=$request;
            """;
        revoke.Parameters.AddWithValue("$now", now);
        revoke.Parameters.AddWithValue("$entry", entryId.ToString());
        revoke.Parameters.AddWithValue("$request", requestId.ToString());
        revoke.Parameters.AddWithValue("$actor", actor);
        await revoke.ExecuteNonQueryAsync(cancellationToken);
        await BumpPolicyAsync(connection, transaction, actor, "request_revoked", "AccessRequest", requestId.ToString(), cancellationToken);
        await transaction.CommitAsync(cancellationToken);
        return true;
    }

    private static async Task EnsureInventorySchemaAsync(SqliteConnection connection, CancellationToken cancellationToken)
    {
        await EnsureColumnAsync(connection, "workstations", "description", "TEXT", cancellationToken);
        await EnsureColumnAsync(connection, "events", "usb_device_id", "TEXT REFERENCES usb_devices(id)", cancellationToken);
        await EnsureColumnAsync(connection, "events", "workstation_device_id", "TEXT REFERENCES workstation_devices(id)", cancellationToken);
        await EnsureColumnAsync(connection, "access_requests", "usb_device_id", "TEXT REFERENCES usb_devices(id)", cancellationToken);
        await EnsureColumnAsync(connection, "access_requests", "workstation_device_id", "TEXT REFERENCES workstation_devices(id)", cancellationToken);
    }

    private static async Task EnsureColumnAsync(SqliteConnection connection, string table, string column, string definition, CancellationToken cancellationToken)
    {
        await using SqliteCommand inspect = connection.CreateCommand();
        inspect.CommandText = $"PRAGMA table_info({table});";
        await using SqliteDataReader reader = await inspect.ExecuteReaderAsync(cancellationToken);
        while (await reader.ReadAsync(cancellationToken))
        {
            if (string.Equals(reader.GetString(1), column, StringComparison.OrdinalIgnoreCase))
                return;
        }
        await reader.DisposeAsync();
        await ExecuteAsync(connection, $"ALTER TABLE {table} ADD COLUMN {column} {definition};", cancellationToken);
    }

    private static async Task<(Guid DeviceId, Guid PairId)> UpsertInventoryAsync(
        SqliteConnection connection,
        SqliteTransaction transaction,
        Guid workstationId,
        DeviceSnapshot device,
        DateTimeOffset observedAt,
        string? eventType,
        string? decision,
        string? reasonCode,
        long policyVersion,
        CancellationToken cancellationToken)
    {
        string instance = NormalizeIdentity(device.InstanceId);
        string? serial = string.IsNullOrWhiteSpace(device.Serial) ? null : NormalizeIdentity(device.Serial);
        string identityKind = serial is null ? "workstation_instance" : "serial";
        string identityConfidence = serial is null ? "local" : "strong";
        string identityMaterial = serial is null
            ? $"LOCAL|{workstationId:N}|{instance}"
            : $"SERIAL|{NormalizeIdentity(device.Transport)}|{NormalizeIdentity(device.Vid)}|{NormalizeIdentity(device.Pid)}|{serial}";
        string identityHash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(identityMaterial)));
        string now = Utc(observedAt.ToUniversalTime());
        string searchText = NormalizeSearch(string.Join(' ', new[]
        {
            device.Vid, device.Pid, device.Serial, device.FriendlyName, device.Manufacturer,
            device.Model, device.Description, device.InstanceId,
        }.Where(value => !string.IsNullOrWhiteSpace(value))));

        Guid proposedDeviceId = Guid.NewGuid();
        await using (SqliteCommand upsertDevice = connection.CreateCommand())
        {
            upsertDevice.Transaction = transaction;
            upsertDevice.CommandText = """
                INSERT INTO usb_devices(id,identity_kind,identity_confidence,identity_hash,vid,pid,serial,
                    friendly_name,manufacturer,model,observed_description,class_guid,service,transport,
                    capabilities_json,search_text,first_seen_utc,last_seen_utc,created_at_utc,updated_at_utc)
                VALUES($id,$kind,$confidence,$hash,$vid,$pid,$serial,$name,$manufacturer,$model,$description,
                    $class,$service,$transport,$capabilities,$search,$seen,$seen,$seen,$seen)
                ON CONFLICT(identity_hash) DO UPDATE SET
                    friendly_name=excluded.friendly_name,
                    manufacturer=COALESCE(excluded.manufacturer,usb_devices.manufacturer),
                    model=COALESCE(excluded.model,usb_devices.model),
                    observed_description=COALESCE(excluded.observed_description,usb_devices.observed_description),
                    class_guid=excluded.class_guid,service=COALESCE(excluded.service,usb_devices.service),
                    capabilities_json=excluded.capabilities_json,search_text=excluded.search_text,
                    last_seen_utc=CASE WHEN excluded.last_seen_utc>usb_devices.last_seen_utc THEN excluded.last_seen_utc ELSE usb_devices.last_seen_utc END,
                    updated_at_utc=excluded.updated_at_utc;
                """;
            upsertDevice.Parameters.AddWithValue("$id", proposedDeviceId.ToString());
            upsertDevice.Parameters.AddWithValue("$kind", identityKind);
            upsertDevice.Parameters.AddWithValue("$confidence", identityConfidence);
            upsertDevice.Parameters.AddWithValue("$hash", identityHash);
            upsertDevice.Parameters.AddWithValue("$vid", NormalizeIdentity(device.Vid));
            upsertDevice.Parameters.AddWithValue("$pid", NormalizeIdentity(device.Pid));
            upsertDevice.Parameters.AddWithValue("$serial", Db(serial));
            upsertDevice.Parameters.AddWithValue("$name", device.FriendlyName);
            upsertDevice.Parameters.AddWithValue("$manufacturer", Db(TrimOrNull(device.Manufacturer)));
            upsertDevice.Parameters.AddWithValue("$model", Db(TrimOrNull(device.Model)));
            upsertDevice.Parameters.AddWithValue("$description", Db(TrimOrNull(device.Description)));
            upsertDevice.Parameters.AddWithValue("$class", device.ClassGuid);
            upsertDevice.Parameters.AddWithValue("$service", Db(device.Service));
            upsertDevice.Parameters.AddWithValue("$transport", device.Transport);
            upsertDevice.Parameters.AddWithValue("$capabilities", JsonSerializer.Serialize(device.Capabilities, PolicyJson.Options));
            upsertDevice.Parameters.AddWithValue("$search", searchText);
            upsertDevice.Parameters.AddWithValue("$seen", now);
            await upsertDevice.ExecuteNonQueryAsync(cancellationToken);
        }

        Guid deviceId;
        await using (SqliteCommand findDevice = connection.CreateCommand())
        {
            findDevice.Transaction = transaction;
            findDevice.CommandText = "SELECT id FROM usb_devices WHERE identity_hash=$hash;";
            findDevice.Parameters.AddWithValue("$hash", identityHash);
            deviceId = Guid.Parse((string)(await findDevice.ExecuteScalarAsync(cancellationToken))!);
        }

        Guid proposedPairId = Guid.NewGuid();
        await using (SqliteCommand upsertPair = connection.CreateCommand())
        {
            upsertPair.Transaction = transaction;
            upsertPair.CommandText = """
                INSERT INTO workstation_devices(id,workstation_id,usb_device_id,instance_id,normalized_instance_id,
                    snapshot_json,hardware_ids_json,device_install_ids_json,first_seen_utc,last_seen_utc,
                    last_event_type,last_decision,last_reason_code,last_policy_version,last_connected,last_observed_at_utc)
                VALUES($id,$ws,$device,$instance,$normalized,$snapshot,$hardware,$install,$seen,$seen,
                    $event,$decision,$reason,$policy,$connected,$seen)
                ON CONFLICT(workstation_id,normalized_instance_id) DO UPDATE SET
                    usb_device_id=excluded.usb_device_id,instance_id=excluded.instance_id,
                    snapshot_json=excluded.snapshot_json,hardware_ids_json=excluded.hardware_ids_json,
                    device_install_ids_json=excluded.device_install_ids_json,
                    last_seen_utc=CASE WHEN excluded.last_seen_utc>workstation_devices.last_seen_utc THEN excluded.last_seen_utc ELSE workstation_devices.last_seen_utc END,
                    last_event_type=COALESCE(excluded.last_event_type,workstation_devices.last_event_type),
                    last_decision=COALESCE(excluded.last_decision,workstation_devices.last_decision),
                    last_reason_code=COALESCE(excluded.last_reason_code,workstation_devices.last_reason_code),
                    last_policy_version=MAX(excluded.last_policy_version,workstation_devices.last_policy_version),
                    last_connected=COALESCE(excluded.last_connected,workstation_devices.last_connected),
                    last_observed_at_utc=CASE WHEN excluded.last_observed_at_utc>COALESCE(workstation_devices.last_observed_at_utc,'') THEN excluded.last_observed_at_utc ELSE workstation_devices.last_observed_at_utc END;
                """;
            upsertPair.Parameters.AddWithValue("$id", proposedPairId.ToString());
            upsertPair.Parameters.AddWithValue("$ws", workstationId.ToString());
            upsertPair.Parameters.AddWithValue("$device", deviceId.ToString());
            upsertPair.Parameters.AddWithValue("$instance", device.InstanceId);
            upsertPair.Parameters.AddWithValue("$normalized", instance);
            upsertPair.Parameters.AddWithValue("$snapshot", JsonSerializer.Serialize(device, PolicyJson.Options));
            upsertPair.Parameters.AddWithValue("$hardware", JsonSerializer.Serialize(device.HardwareIds, PolicyJson.Options));
            upsertPair.Parameters.AddWithValue("$install", JsonSerializer.Serialize(device.DeviceInstallIds, PolicyJson.Options));
            upsertPair.Parameters.AddWithValue("$seen", now);
            upsertPair.Parameters.AddWithValue("$event", Db(eventType));
            upsertPair.Parameters.AddWithValue("$decision", Db(decision));
            upsertPair.Parameters.AddWithValue("$reason", Db(reasonCode));
            upsertPair.Parameters.AddWithValue("$policy", policyVersion);
            bool? connected = eventType is "removed" or "disconnected" ? false : eventType is null or "access_request" ? null : true;
            upsertPair.Parameters.AddWithValue("$connected", connected is null ? DBNull.Value : connected.Value);
            await upsertPair.ExecuteNonQueryAsync(cancellationToken);
        }

        await using SqliteCommand findPair = connection.CreateCommand();
        findPair.Transaction = transaction;
        findPair.CommandText = "SELECT id FROM workstation_devices WHERE workstation_id=$ws AND normalized_instance_id=$instance;";
        findPair.Parameters.AddWithValue("$ws", workstationId.ToString());
        findPair.Parameters.AddWithValue("$instance", instance);
        Guid pairId = Guid.Parse((string)(await findPair.ExecuteScalarAsync(cancellationToken))!);
        return (deviceId, pairId);
    }

    private async Task BackfillInventoryAsync(CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        List<(string Source, Guid Id, Guid WorkstationId, DeviceSnapshot Device, DateTimeOffset Seen, string? Type, string? Decision, string? Reason, long Policy)> rows = [];
        await using (SqliteCommand command = connection.CreateCommand())
        {
            command.CommandText = """
                SELECT 'event',id,workstation_id,device_json,occurred_at_utc,type,decision,reason_code,policy_version
                FROM events WHERE device_json IS NOT NULL AND workstation_device_id IS NULL
                UNION ALL
                SELECT 'request',id,workstation_id,device_json,created_at_utc,'access_request',NULL,NULL,0
                FROM access_requests WHERE workstation_device_id IS NULL;
                """;
            await using SqliteDataReader reader = await command.ExecuteReaderAsync(cancellationToken);
            while (await reader.ReadAsync(cancellationToken))
            {
                DeviceSnapshot? device = JsonSerializer.Deserialize<DeviceSnapshot>(reader.GetString(3), PolicyJson.Options);
                if (device is not null)
                    rows.Add((reader.GetString(0), Guid.Parse(reader.GetString(1)), Guid.Parse(reader.GetString(2)), device,
                        ParseUtc(reader.GetString(4)), NullableString(reader, 5), NullableString(reader, 6), NullableString(reader, 7), reader.GetInt64(8)));
            }
        }

        foreach (var row in rows)
        {
            await using SqliteTransaction transaction = (SqliteTransaction)await connection.BeginTransactionAsync(cancellationToken);
            (Guid deviceId, Guid pairId) = await UpsertInventoryAsync(connection, transaction, row.WorkstationId, row.Device,
                row.Seen, row.Type, row.Decision, row.Reason, row.Policy, cancellationToken);
            await using SqliteCommand update = connection.CreateCommand();
            update.Transaction = transaction;
            update.CommandText = $"UPDATE {(row.Source == "event" ? "events" : "access_requests")} SET usb_device_id=$device,workstation_device_id=$pair WHERE id=$id;";
            update.Parameters.AddWithValue("$device", deviceId.ToString());
            update.Parameters.AddWithValue("$pair", pairId.ToString());
            update.Parameters.AddWithValue("$id", row.Id.ToString());
            await update.ExecuteNonQueryAsync(cancellationToken);
            await transaction.CommitAsync(cancellationToken);
        }
    }

    private async Task<SqliteConnection> OpenAsync(CancellationToken cancellationToken)
    {
        SqliteConnection connection = new(_connectionString);
        await connection.OpenAsync(cancellationToken);
        await using SqliteCommand command = connection.CreateCommand();
        command.CommandText = "PRAGMA foreign_keys=ON; PRAGMA busy_timeout=5000;";
        await command.ExecuteNonQueryAsync(cancellationToken);
        return connection;
    }

    private static async Task ExecuteAsync(SqliteConnection connection, string sql, CancellationToken cancellationToken)
    {
        await using SqliteCommand command = connection.CreateCommand();
        command.CommandText = sql;
        await command.ExecuteNonQueryAsync(cancellationToken);
    }

    private static async Task<long> PolicyVersionAsync(SqliteConnection connection, CancellationToken cancellationToken) =>
        await PolicyVersionAsync(connection, null, cancellationToken);

    private static async Task<long> PolicyVersionAsync(SqliteConnection connection, SqliteTransaction? transaction, CancellationToken cancellationToken)
    {
        await using SqliteCommand command = connection.CreateCommand();
        command.Transaction = transaction;
        command.CommandText = "SELECT value FROM meta WHERE key='policy_version';";
        return long.Parse((string)(await command.ExecuteScalarAsync(cancellationToken) ?? "1"), CultureInfo.InvariantCulture);
    }

    private static async Task BumpPolicyAsync(SqliteConnection connection, SqliteTransaction transaction, string actor, string action, string targetType, string targetId, CancellationToken cancellationToken)
    {
        await using SqliteCommand version = connection.CreateCommand();
        version.Transaction = transaction;
        version.CommandText = "UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='policy_version';";
        await version.ExecuteNonQueryAsync(cancellationToken);
        await using SqliteCommand audit = connection.CreateCommand();
        audit.Transaction = transaction;
        audit.CommandText = "INSERT INTO audit_logs(id,actor,action,target_type,target_id,occurred_at_utc) VALUES($id,$actor,$action,$type,$target,$now);";
        audit.Parameters.AddWithValue("$id", Guid.NewGuid().ToString());
        audit.Parameters.AddWithValue("$actor", actor);
        audit.Parameters.AddWithValue("$action", action);
        audit.Parameters.AddWithValue("$type", targetType);
        audit.Parameters.AddWithValue("$target", targetId);
        audit.Parameters.AddWithValue("$now", Utc(DateTimeOffset.UtcNow));
        await audit.ExecuteNonQueryAsync(cancellationToken);
    }

    private static DeviceEntry ReadEntry(SqliteDataReader reader) => new(
        Guid.Parse(reader.GetString(0)),
        Enum.Parse<MatchType>(Pascal(reader.GetString(1))),
        NullableString(reader, 2), NullableString(reader, 3), NullableString(reader, 4), NullableString(reader, 5),
        reader.IsDBNull(6) ? null : Enum.Parse<ProductDeviceClass>(Pascal(reader.GetString(6))),
        reader.GetString(7),
        Enum.Parse<EntryAction>(Pascal(reader.GetString(8))),
        reader.GetBoolean(9),
        Enum.Parse<ScopeType>(Pascal(reader.GetString(10))),
        reader.IsDBNull(11) ? null : Guid.Parse(reader.GetString(11)),
        reader.IsDBNull(12) ? null : ParseUtc(reader.GetString(12)),
        NullableString(reader, 13),
        JsonSerializer.Deserialize<string[]>(reader.GetString(14), PolicyJson.Options) ?? []);

    private static void AddEntryParameters(SqliteCommand command, DeviceEntry entry)
    {
        command.Parameters.AddWithValue("$id", entry.Id.ToString());
        command.Parameters.AddWithValue("$match", Snake(entry.MatchType.ToString()));
        command.Parameters.AddWithValue("$vid", Db(entry.Vid)); command.Parameters.AddWithValue("$pid", Db(entry.Pid));
        command.Parameters.AddWithValue("$serial", Db(entry.Serial)); command.Parameters.AddWithValue("$instance", Db(entry.InstanceId));
        command.Parameters.AddWithValue("$class", Db(entry.DeviceClass is null ? null : Snake(entry.DeviceClass.ToString()!)));
        command.Parameters.AddWithValue("$name", entry.DisplayName); command.Parameters.AddWithValue("$action", Snake(entry.Action.ToString()));
        command.Parameters.AddWithValue("$mandatory", entry.Mandatory); command.Parameters.AddWithValue("$scope", Snake(entry.ScopeType.ToString()));
        command.Parameters.AddWithValue("$scopeId", Db(entry.ScopeId?.ToString())); command.Parameters.AddWithValue("$expires", Db(entry.ExpiresAtUtc is null ? null : Utc(entry.ExpiresAtUtc.Value)));
        command.Parameters.AddWithValue("$note", Db(entry.Note)); command.Parameters.AddWithValue("$install", JsonSerializer.Serialize(entry.DeviceInstallIds, PolicyJson.Options));
        command.Parameters.AddWithValue("$now", Utc(DateTimeOffset.UtcNow));
    }

    private static async Task<List<WorkstationRow>> WorkstationsAsync(SqliteConnection connection, CancellationToken cancellationToken)
    {
        List<WorkstationRow> rows = [];
        await using SqliteCommand command = connection.CreateCommand();
        command.CommandText = "SELECT w.id,w.hostname,w.machine_sid,g.name,w.agent_version,w.last_seen_utc,w.applied_policy_version,w.health,w.description FROM workstations w JOIN groups g ON g.id=w.group_id ORDER BY w.hostname;";
        await using SqliteDataReader reader = await command.ExecuteReaderAsync(cancellationToken);
        while (await reader.ReadAsync(cancellationToken)) rows.Add(new(Guid.Parse(reader.GetString(0)),reader.GetString(1),reader.GetString(2),reader.GetString(3),reader.GetString(4),reader.IsDBNull(5)?null:ParseUtc(reader.GetString(5)),reader.GetInt64(6),reader.GetString(7),NullableString(reader,8)));
        return rows;
    }

    private static async Task<List<PolicyRow>> PoliciesAsync(SqliteConnection connection, CancellationToken cancellationToken)
    {
        List<PolicyRow> rows = [];
        await using SqliteCommand command = connection.CreateCommand();
        command.CommandText = "SELECT id,display_name,match_type,COALESCE(instance_id,vid||':'||pid||COALESCE(':'||serial,''),device_class),action,scope_type,scope_id,mandatory,expires_at_utc FROM device_entries WHERE deleted_at_utc IS NULL ORDER BY created_at_utc DESC;";
        await using SqliteDataReader reader = await command.ExecuteReaderAsync(cancellationToken);
        while(await reader.ReadAsync(cancellationToken)) rows.Add(new(Guid.Parse(reader.GetString(0)),reader.GetString(1),Enum.Parse<MatchType>(Pascal(reader.GetString(2))),reader.IsDBNull(3)?string.Empty:reader.GetString(3),Enum.Parse<EntryAction>(Pascal(reader.GetString(4))),Enum.Parse<ScopeType>(Pascal(reader.GetString(5))),reader.IsDBNull(6)?null:Guid.Parse(reader.GetString(6)),reader.GetBoolean(7),reader.IsDBNull(8)?null:ParseUtc(reader.GetString(8))));
        return rows;
    }

    private static async Task<List<RequestRow>> RequestsAsync(SqliteConnection connection, CancellationToken cancellationToken)
    {
        List<RequestRow> rows=[]; await using SqliteCommand command=connection.CreateCommand();
        command.CommandText="SELECT r.id,r.workstation_id,w.hostname,r.username,json_extract(r.device_json,'$.friendlyName'),r.instance_id,r.requested_duration_days,r.justification,r.status,r.created_at_utc FROM access_requests r JOIN workstations w ON w.id=r.workstation_id ORDER BY r.created_at_utc DESC LIMIT 200;";
        await using SqliteDataReader reader=await command.ExecuteReaderAsync(cancellationToken);
        while(await reader.ReadAsync(cancellationToken)) rows.Add(new(Guid.Parse(reader.GetString(0)),Guid.Parse(reader.GetString(1)),reader.GetString(2),reader.GetString(3),reader.GetString(4),reader.GetString(5),reader.GetInt32(6),reader.GetString(7),reader.GetString(8),ParseUtc(reader.GetString(9)))); return rows;
    }

    private static async Task<List<EventRow>> EventsAsync(SqliteConnection connection, CancellationToken cancellationToken)
    {
        List<EventRow> rows=[]; await using SqliteCommand command=connection.CreateCommand();
        command.CommandText="SELECT id,type,occurred_at_utc,hostname,username,json_extract(device_json,'$.friendlyName'),json_extract(device_json,'$.instanceId'),decision,reason_code FROM events ORDER BY occurred_at_utc DESC LIMIT 500;";
        await using SqliteDataReader reader=await command.ExecuteReaderAsync(cancellationToken);
        while(await reader.ReadAsync(cancellationToken)) rows.Add(new(Guid.Parse(reader.GetString(0)),reader.GetString(1),ParseUtc(reader.GetString(2)),reader.GetString(3),NullableString(reader,4),NullableString(reader,5),NullableString(reader,6),NullableString(reader,7),NullableString(reader,8))); return rows;
    }

    private static async Task<List<AuditRow>> AuditAsync(SqliteConnection connection, CancellationToken cancellationToken)
    {
        List<AuditRow> rows=[]; await using SqliteCommand command=connection.CreateCommand(); command.CommandText="SELECT id,actor,action,target_type,target_id,occurred_at_utc FROM audit_logs ORDER BY occurred_at_utc DESC LIMIT 200;";
        await using SqliteDataReader reader=await command.ExecuteReaderAsync(cancellationToken); while(await reader.ReadAsync(cancellationToken)) rows.Add(new(Guid.Parse(reader.GetString(0)),reader.GetString(1),reader.GetString(2),reader.GetString(3),reader.GetString(4),ParseUtc(reader.GetString(5)))); return rows;
    }

    private static string BuildTokenPredicate(string normalized, string expression, out string[] terms)
    {
        terms = normalized.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        return terms.Length == 0 ? "1=1" : string.Join(" AND ", terms.Select((_, index) => $"{expression} LIKE $term{index}"));
    }

    private static void AddSearchParameters(SqliteCommand command, IReadOnlyList<string> terms)
    {
        for (int index = 0; index < terms.Count; index++) command.Parameters.AddWithValue($"$term{index}", $"%{terms[index]}%");
    }

    private static async Task<int> CountAsync(SqliteConnection connection, string sql, IReadOnlyList<string> terms, CancellationToken cancellationToken)
    {
        await using SqliteCommand command = connection.CreateCommand(); command.CommandText = sql; AddSearchParameters(command, terms);
        return Convert.ToInt32(await command.ExecuteScalarAsync(cancellationToken), CultureInfo.InvariantCulture);
    }

    private static DeviceInventoryRow ReadDeviceRow(SqliteDataReader reader) => new(
        Guid.Parse(reader.GetString(0)), reader.GetString(1), reader.GetString(2), reader.GetString(3),
        NullableString(reader, 4), reader.GetString(5), NullableString(reader, 6), NullableString(reader, 7),
        NullableString(reader, 8), ParseUtc(reader.GetString(9)), ParseUtc(reader.GetString(10)), reader.GetInt32(11));

    private static WorkstationInventoryRow ReadWorkstationInventoryRow(SqliteDataReader reader) => new(
        Guid.Parse(reader.GetString(0)), reader.GetString(1), reader.GetString(2), reader.GetString(3),
        NullableString(reader, 4), reader.GetString(5), reader.IsDBNull(6) ? null : ParseUtc(reader.GetString(6)),
        reader.GetString(7), reader.GetInt32(8));

    private static async Task<List<Guid>> RelatedIdsAsync(SqliteConnection connection, string sql, Guid id, CancellationToken cancellationToken)
    {
        List<Guid> ids = []; await using SqliteCommand command = connection.CreateCommand(); command.CommandText = sql;
        command.Parameters.AddWithValue("$id", id.ToString()); await using SqliteDataReader reader = await command.ExecuteReaderAsync(cancellationToken);
        while (await reader.ReadAsync(cancellationToken)) ids.Add(Guid.Parse(reader.GetString(0))); return ids;
    }

    private static async Task<List<RequestRow>> RequestsForPairAsync(SqliteConnection connection, Guid workstationId, string instanceId, CancellationToken cancellationToken)
    {
        List<RequestRow> rows = []; await using SqliteCommand command = connection.CreateCommand();
        command.CommandText = """
            SELECT r.id,r.workstation_id,w.hostname,r.username,json_extract(r.device_json,'$.friendlyName'),r.instance_id,
                   r.requested_duration_days,r.justification,r.status,r.created_at_utc
            FROM access_requests r JOIN workstations w ON w.id=r.workstation_id
            WHERE r.workstation_id=$ws AND r.instance_id=$instance ORDER BY r.created_at_utc DESC LIMIT 20;
            """;
        command.Parameters.AddWithValue("$ws", workstationId.ToString()); command.Parameters.AddWithValue("$instance", instanceId);
        await using SqliteDataReader reader = await command.ExecuteReaderAsync(cancellationToken);
        while (await reader.ReadAsync(cancellationToken)) rows.Add(new(Guid.Parse(reader.GetString(0)), Guid.Parse(reader.GetString(1)), reader.GetString(2), reader.GetString(3), reader.GetString(4), reader.GetString(5), reader.GetInt32(6), reader.GetString(7), reader.GetString(8), ParseUtc(reader.GetString(9))));
        return rows;
    }

    private static async Task<DeviceSnapshot?> PairSnapshotAsync(SqliteConnection connection, Guid workstationId, Guid deviceId, CancellationToken cancellationToken)
    {
        await using SqliteCommand command = connection.CreateCommand();
        command.CommandText = "SELECT snapshot_json FROM workstation_devices WHERE workstation_id=$ws AND usb_device_id=$device;";
        command.Parameters.AddWithValue("$ws", workstationId.ToString()); command.Parameters.AddWithValue("$device", deviceId.ToString());
        string? json = (string?)await command.ExecuteScalarAsync(cancellationToken);
        return json is null ? null : JsonSerializer.Deserialize<DeviceSnapshot>(json, PolicyJson.Options);
    }

    private static DeviceInfo SnapshotToDeviceInfo(DeviceSnapshot snapshot) => new(
        snapshot.InstanceId, snapshot.HardwareIds, snapshot.DeviceInstallIds, snapshot.Vid, snapshot.Pid,
        snapshot.Serial, snapshot.Capabilities.Contains("usb_storage", StringComparer.OrdinalIgnoreCase)
            ? ProductDeviceClass.UsbStorage : ProductDeviceClass.Wpd,
        snapshot.ClassGuid, snapshot.Service, snapshot.FriendlyName, snapshot.Transport, snapshot.Capabilities, [],
        snapshot.Manufacturer, snapshot.Model, snapshot.Description);

    private static byte[] HashCredential(byte[] salt, string secret) => SHA256.HashData([.. salt, .. Encoding.UTF8.GetBytes(secret)]);
    private static string Base64Url(byte[] value) => Convert.ToBase64String(value).TrimEnd('=').Replace('+', '-').Replace('/', '_');
    private static string NormalizeIdentity(string value) => value.Trim().ToUpperInvariant();
    private static string NormalizeSearch(string value) => string.Join(' ', value.Trim().ToUpperInvariant().Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries));
    private static string? TrimOrNull(string? value) => string.IsNullOrWhiteSpace(value) ? null : value.Trim();
    private static object Db(string? value) => value is null ? DBNull.Value : value;
    private static string? NullableString(SqliteDataReader reader, int ordinal) => reader.IsDBNull(ordinal) ? null : reader.GetString(ordinal);
    private static string Utc(DateTimeOffset value) => value.ToUniversalTime().ToString("O", CultureInfo.InvariantCulture);
    private static DateTimeOffset ParseUtc(string value) => DateTimeOffset.Parse(value, CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind).ToUniversalTime();
    private static string Snake(string value) => string.Concat(value.Select((character,index) => index>0 && char.IsUpper(character) ? "_"+char.ToLowerInvariant(character) : char.ToLowerInvariant(character).ToString()));
    private static string Pascal(string value) => string.Concat(value.Split('_').Select(part => char.ToUpperInvariant(part[0])+part[1..]));
    private static GroupMode ParseGroupMode(string value) => value == "default_allow" ? GroupMode.DefaultAllow : GroupMode.DefaultDeny;
    public static readonly Guid DefaultGroupId = Guid.Parse("10000000-0000-0000-0000-000000000001");

    private const string Schema = """
        CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS groups(id TEXT PRIMARY KEY,name TEXT NOT NULL UNIQUE,mode TEXT NOT NULL,created_at_utc TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS workstations(id TEXT PRIMARY KEY,machine_sid TEXT NOT NULL UNIQUE,hostname TEXT NOT NULL,group_id TEXT NOT NULL REFERENCES groups(id),key_salt BLOB NOT NULL,key_hash BLOB NOT NULL,key_version INTEGER NOT NULL,agent_version TEXT NOT NULL,enrolled_at_utc TEXT NOT NULL,last_seen_utc TEXT,applied_policy_version INTEGER NOT NULL,health TEXT NOT NULL,last_error TEXT,revoked_at_utc TEXT,description TEXT);
        CREATE TABLE IF NOT EXISTS enrollment_tokens(id TEXT PRIMARY KEY,token_hash BLOB NOT NULL UNIQUE,created_at_utc TEXT NOT NULL,expires_at_utc TEXT NOT NULL,revoked_at_utc TEXT,max_uses INTEGER NOT NULL,use_count INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS device_entries(id TEXT PRIMARY KEY,match_type TEXT NOT NULL,vid TEXT,pid TEXT,serial TEXT,instance_id TEXT,device_class TEXT,display_name TEXT NOT NULL,action TEXT NOT NULL,mandatory INTEGER NOT NULL,scope_type TEXT NOT NULL,scope_id TEXT,expires_at_utc TEXT,note TEXT,device_install_ids_json TEXT NOT NULL,created_at_utc TEXT NOT NULL,updated_at_utc TEXT NOT NULL,deleted_at_utc TEXT);
        CREATE TABLE IF NOT EXISTS usb_devices(id TEXT PRIMARY KEY,identity_kind TEXT NOT NULL,identity_confidence TEXT NOT NULL,identity_hash TEXT NOT NULL UNIQUE,vid TEXT NOT NULL,pid TEXT NOT NULL,serial TEXT,friendly_name TEXT NOT NULL,manufacturer TEXT,model TEXT,observed_description TEXT,admin_description TEXT,class_guid TEXT NOT NULL,service TEXT,transport TEXT NOT NULL,capabilities_json TEXT NOT NULL,search_text TEXT NOT NULL,first_seen_utc TEXT NOT NULL,last_seen_utc TEXT NOT NULL,created_at_utc TEXT NOT NULL,updated_at_utc TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS workstation_devices(id TEXT PRIMARY KEY,workstation_id TEXT NOT NULL REFERENCES workstations(id),usb_device_id TEXT NOT NULL REFERENCES usb_devices(id),instance_id TEXT NOT NULL,normalized_instance_id TEXT NOT NULL,snapshot_json TEXT NOT NULL,hardware_ids_json TEXT NOT NULL,device_install_ids_json TEXT NOT NULL,first_seen_utc TEXT NOT NULL,last_seen_utc TEXT NOT NULL,last_event_type TEXT,last_decision TEXT,last_reason_code TEXT,last_policy_version INTEGER NOT NULL,last_connected INTEGER,last_observed_at_utc TEXT,UNIQUE(workstation_id,normalized_instance_id));
        CREATE TABLE IF NOT EXISTS events(id TEXT PRIMARY KEY,workstation_id TEXT NOT NULL REFERENCES workstations(id),client_event_id TEXT NOT NULL,type TEXT NOT NULL,occurred_at_utc TEXT NOT NULL,received_at_utc TEXT NOT NULL,hostname TEXT NOT NULL,user_sid TEXT,username TEXT,device_json TEXT,policy_version INTEGER NOT NULL,decision TEXT,matched_entry_id TEXT,reason_code TEXT,agent_version TEXT NOT NULL,details_json TEXT NOT NULL,usb_device_id TEXT REFERENCES usb_devices(id),workstation_device_id TEXT REFERENCES workstation_devices(id),UNIQUE(workstation_id,client_event_id));
        CREATE TABLE IF NOT EXISTS access_requests(id TEXT PRIMARY KEY,client_request_id TEXT NOT NULL,workstation_id TEXT NOT NULL REFERENCES workstations(id),user_sid TEXT NOT NULL,username TEXT NOT NULL,device_json TEXT NOT NULL,instance_id TEXT NOT NULL,requested_duration_days INTEGER NOT NULL,justification TEXT NOT NULL,status TEXT NOT NULL,created_at_utc TEXT NOT NULL,decision_reason TEXT,decided_by TEXT,decided_at_utc TEXT,resulting_entry_id TEXT,usb_device_id TEXT REFERENCES usb_devices(id),workstation_device_id TEXT REFERENCES workstation_devices(id),UNIQUE(workstation_id,client_request_id));
        CREATE TABLE IF NOT EXISTS audit_logs(id TEXT PRIMARY KEY,actor TEXT NOT NULL,action TEXT NOT NULL,target_type TEXT NOT NULL,target_id TEXT NOT NULL,occurred_at_utc TEXT NOT NULL);
        CREATE INDEX IF NOT EXISTS ix_usb_devices_identity ON usb_devices(identity_hash);
        CREATE INDEX IF NOT EXISTS ix_usb_devices_search ON usb_devices(search_text);
        CREATE INDEX IF NOT EXISTS ix_usb_devices_last_seen ON usb_devices(last_seen_utc DESC);
        CREATE INDEX IF NOT EXISTS ix_workstation_devices_device ON workstation_devices(usb_device_id,last_seen_utc DESC);
        CREATE INDEX IF NOT EXISTS ix_workstation_devices_workstation ON workstation_devices(workstation_id,last_seen_utc DESC);
        CREATE INDEX IF NOT EXISTS ix_workstations_hostname ON workstations(hostname);
        """;
}
