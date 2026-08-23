using System.Globalization;
using System.Text.Json;
using UsbDlp.Policy;
using UsbDlp.Windows;

return await UsbDlpCli.RunAsync(args);

internal static class UsbDlpCli
{
    private const int Success = 0;
    private const int UsageError = 2;
    private const int InputError = 3;
    private const int PolicyError = 4;

    internal static async Task<int> RunAsync(string[] args)
    {
        try
        {
            if (args.Length > 0 && string.Equals(args[0], "monitor", StringComparison.Ordinal))
            {
                return await RunMonitorAsync(args);
            }

            if (args.Length > 0 && string.Equals(args[0], "enforce", StringComparison.Ordinal))
            {
                return await RunEnforceAsync(args);
            }

            if (args.SequenceEqual(["registry-clear-owned", "--live"], StringComparer.Ordinal))
            {
                RegistryPolicyResult result = new RegistryPolicyManager().Clear(dryRun: false);
                Console.WriteLine(JsonSerializer.Serialize(result, PolicyJson.Options));
                return Success;
            }

            if (!TryParseEvaluate(args, out EvaluateOptions options, out string? parseError))
            {
                Console.Error.WriteLine(parseError);
                WriteUsage();
                return UsageError;
            }

            DeviceInfo device = await ReadJsonAsync<DeviceInfo>(options.DevicePath);
            CompiledPolicy policy = await ReadJsonAsync<CompiledPolicy>(options.PolicyPath);
            DateTimeOffset evaluatedAtUtc = options.EvaluatedAtUtc ?? DateTimeOffset.UtcNow;
            PolicyEvaluation evaluation = PolicyEvaluator.Evaluate(device, policy, evaluatedAtUtc);

            Console.WriteLine(JsonSerializer.Serialize(evaluation, PolicyJson.Options));
            return Success;
        }
        catch (PolicyValidationException exception)
        {
            Console.Error.WriteLine(exception.Message);
            return PolicyError;
        }
        catch (IOException exception)
        {
            Console.Error.WriteLine($"Input error: {exception.Message}");
            return InputError;
        }
        catch (UnauthorizedAccessException exception)
        {
            Console.Error.WriteLine($"Input error: {exception.Message}");
            return InputError;
        }
        catch (JsonException exception)
        {
            Console.Error.WriteLine($"Invalid JSON: {exception.Message}");
            return InputError;
        }
    }

    private static async Task<int> RunMonitorAsync(string[] args)
    {
        bool watch = args.Skip(1).Contains("--watch", StringComparer.Ordinal);
        if (args.Skip(1).Any(argument => argument != "--watch"))
        {
            Console.Error.WriteLine("Usage: UsbDlp.Cli monitor [--watch]");
            return UsageError;
        }

        WindowsDeviceEnumerator enumerator = new();
        HashSet<string> previous = new(StringComparer.OrdinalIgnoreCase);
        do
        {
            IReadOnlyList<DeviceInfo> devices = enumerator.EnumerateInScopeDevices();
            foreach (DeviceInfo device in devices.Where(device => previous.Add(device.InstanceId)))
            {
                Console.WriteLine(JsonSerializer.Serialize(device, PolicyJson.Options));
            }
            previous.IntersectWith(devices.Select(device => device.InstanceId));
            if (watch)
            {
                await Task.Delay(TimeSpan.FromSeconds(1));
            }
        }
        while (watch);

        return Success;
    }

    private static async Task<int> RunEnforceAsync(string[] args)
    {
        Dictionary<string, string> values = new(StringComparer.Ordinal);
        bool live = args.Contains("--live", StringComparer.Ordinal);
        for (int index = 1; index < args.Length; index++)
        {
            if (args[index] == "--live") continue;
            if (index + 1 >= args.Length || args[index] is not ("--policy" or "--confirm-device-id"))
            {
                Console.Error.WriteLine("Usage: UsbDlp.Cli enforce --policy <policy.json> [--live --confirm-device-id <instance-id>]");
                return UsageError;
            }
            values[args[index]] = args[++index];
        }

        if (!values.TryGetValue("--policy", out string? path) || (live && !values.ContainsKey("--confirm-device-id")))
        {
            Console.Error.WriteLine("Live enforcement requires both --live and --confirm-device-id.");
            return UsageError;
        }

        CompiledPolicy policy = await ReadJsonAsync<CompiledPolicy>(path);
        WindowsDeviceEnumerator enumerator = new();
        WindowsDeviceEnforcer enforcer = new();
        foreach (DeviceInfo device in enumerator.EnumerateInScopeDevices())
        {
            PolicyEvaluation evaluation = PolicyEvaluator.Evaluate(device, policy, DateTimeOffset.UtcNow);
            bool confirmed = !live || string.Equals(values["--confirm-device-id"], device.InstanceId, StringComparison.OrdinalIgnoreCase);
            EnforcementResult result = enforcer.Apply(device, evaluation.Decision, dryRun: !live || !confirmed);
            Console.WriteLine(JsonSerializer.Serialize(new { device = device.InstanceId, evaluation, enforcement = result }, PolicyJson.Options));
        }
        return Success;
    }

    private static async Task<T> ReadJsonAsync<T>(string path)
    {
        await using FileStream stream = File.OpenRead(path);
        T? value = await JsonSerializer.DeserializeAsync<T>(stream, PolicyJson.Options);
        return value ?? throw new JsonException($"'{path}' contains JSON null.");
    }

    private static bool TryParseEvaluate(
        string[] args,
        out EvaluateOptions options,
        out string? error)
    {
        options = null!;
        error = null;

        if (args.Length == 0 || !string.Equals(args[0], "evaluate", StringComparison.Ordinal))
        {
            error = "The only supported command is 'evaluate'.";
            return false;
        }

        Dictionary<string, string> values = new(StringComparer.Ordinal);
        for (int index = 1; index < args.Length; index += 2)
        {
            if (index + 1 >= args.Length || !args[index].StartsWith("--", StringComparison.Ordinal))
            {
                error = $"Missing value for argument '{args[index]}'.";
                return false;
            }

            if (args[index] is not ("--device" or "--policy" or "--at"))
            {
                error = $"Unknown argument '{args[index]}'.";
                return false;
            }

            if (!values.TryAdd(args[index], args[index + 1]))
            {
                error = $"Argument '{args[index]}' was supplied more than once.";
                return false;
            }
        }

        if (!values.TryGetValue("--device", out string? devicePath) ||
            !values.TryGetValue("--policy", out string? policyPath))
        {
            error = "Both --device and --policy are required.";
            return false;
        }

        DateTimeOffset? evaluatedAtUtc = null;
        if (values.TryGetValue("--at", out string? timestamp))
        {
            if (!DateTimeOffset.TryParseExact(
                    timestamp,
                    "O",
                    CultureInfo.InvariantCulture,
                    DateTimeStyles.None,
                    out DateTimeOffset parsed) ||
                parsed.Offset != TimeSpan.Zero)
            {
                error = "--at must be an ISO 8601 round-trip timestamp with UTC offset, for example 2026-08-18T12:00:00.0000000+00:00.";
                return false;
            }

            evaluatedAtUtc = parsed;
        }

        options = new(devicePath, policyPath, evaluatedAtUtc);
        return true;
    }

    private static void WriteUsage() =>
        Console.Error.WriteLine(
            "Usage: UsbDlp.Cli evaluate ... | monitor [--watch] | enforce --policy <policy.json> [--live --confirm-device-id <id>] | registry-clear-owned --live");

    private sealed record EvaluateOptions(
        string DevicePath,
        string PolicyPath,
        DateTimeOffset? EvaluatedAtUtc);
}
