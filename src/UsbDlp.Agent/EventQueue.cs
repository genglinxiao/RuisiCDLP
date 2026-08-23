using System.Text.Json;
using Microsoft.Data.Sqlite;
using UsbDlp.Contracts;
using UsbDlp.Policy;

namespace UsbDlp.Agent;

public sealed class DurableEventStore
{
    private readonly string _connectionString;

    public DurableEventStore(AgentStorage storage) => _connectionString = new SqliteConnectionStringBuilder
    {
        DataSource = storage.EventDatabasePath,
        Mode = SqliteOpenMode.ReadWriteCreate,
        Cache = SqliteCacheMode.Shared,
        Pooling = false,
    }.ToString();

    public async Task InitializeAsync(CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using SqliteCommand command = connection.CreateCommand();
        command.CommandText = """
            PRAGMA journal_mode=WAL;
            CREATE TABLE IF NOT EXISTS queued_events(client_event_id TEXT PRIMARY KEY,json TEXT NOT NULL,created_at_utc TEXT NOT NULL,uploaded_at_utc TEXT);
            """;
        await command.ExecuteNonQueryAsync(cancellationToken);
    }

    public async Task EnqueueAsync(AgentEvent item, CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using SqliteCommand command = connection.CreateCommand();
        command.CommandText = "INSERT OR IGNORE INTO queued_events(client_event_id,json,created_at_utc) VALUES($id,$json,$now);";
        command.Parameters.AddWithValue("$id", item.ClientEventId.ToString());
        command.Parameters.AddWithValue("$json", JsonSerializer.Serialize(item, PolicyJson.Options));
        command.Parameters.AddWithValue("$now", DateTimeOffset.UtcNow.ToString("O"));
        await command.ExecuteNonQueryAsync(cancellationToken);
    }

    public async Task<IReadOnlyList<AgentEvent>> PendingAsync(int count, CancellationToken cancellationToken)
    {
        List<AgentEvent> events = [];
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using SqliteCommand command = connection.CreateCommand();
        command.CommandText = "SELECT json FROM queued_events WHERE uploaded_at_utc IS NULL ORDER BY created_at_utc LIMIT $count;";
        command.Parameters.AddWithValue("$count", count);
        await using SqliteDataReader reader = await command.ExecuteReaderAsync(cancellationToken);
        while (await reader.ReadAsync(cancellationToken))
        {
            AgentEvent? item = JsonSerializer.Deserialize<AgentEvent>(reader.GetString(0), PolicyJson.Options);
            if (item is not null) events.Add(item);
        }
        return events;
    }

    public async Task MarkUploadedAsync(IEnumerable<Guid> ids, CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using SqliteTransaction transaction = (SqliteTransaction)await connection.BeginTransactionAsync(cancellationToken);
        foreach (Guid id in ids)
        {
            await using SqliteCommand command = connection.CreateCommand();
            command.Transaction = transaction;
            command.CommandText = "UPDATE queued_events SET uploaded_at_utc=$now WHERE client_event_id=$id;";
            command.Parameters.AddWithValue("$now", DateTimeOffset.UtcNow.ToString("O"));
            command.Parameters.AddWithValue("$id", id.ToString());
            await command.ExecuteNonQueryAsync(cancellationToken);
        }
        await transaction.CommitAsync(cancellationToken);
    }

    public async Task<int> DepthAsync(CancellationToken cancellationToken)
    {
        await using SqliteConnection connection = await OpenAsync(cancellationToken);
        await using SqliteCommand command = connection.CreateCommand();
        command.CommandText = "SELECT COUNT(*) FROM queued_events WHERE uploaded_at_utc IS NULL;";
        return Convert.ToInt32(await command.ExecuteScalarAsync(cancellationToken), System.Globalization.CultureInfo.InvariantCulture);
    }

    private async Task<SqliteConnection> OpenAsync(CancellationToken cancellationToken)
    {
        SqliteConnection connection = new(_connectionString);
        await connection.OpenAsync(cancellationToken);
        return connection;
    }
}
