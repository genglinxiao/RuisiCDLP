using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32;
using UsbDlp.Policy;

namespace UsbDlp.Windows;

public sealed record RegistryPolicyPlan(
    IReadOnlyDictionary<string, int> ScalarValues,
    IReadOnlyDictionary<string, string> AllowDeviceIds,
    IReadOnlyList<string> Conflicts);

public sealed record RegistryPolicyResult(bool DryRun, int ValuesChanged, IReadOnlyList<string> Conflicts);

public interface IRegistryPolicyManager
{
    RegistryPolicyPlan BuildPlan(CompiledPolicy policy, DateTimeOffset nowUtc);
    RegistryPolicyResult Apply(RegistryPolicyPlan plan, bool dryRun);
    RegistryPolicyResult Clear(bool dryRun);
}

public sealed class RegistryPolicyManager : IRegistryPolicyManager
{
    internal const string RestrictionsPath = @"SOFTWARE\Policies\Microsoft\Windows\DeviceInstall\Restrictions";
    internal const string AllowIdsPath = RestrictionsPath + @"\AllowDeviceIDs";
    private const string OwnershipPath = @"SOFTWARE\UsbDlp\RegistryOwnership";
    private static readonly IReadOnlyDictionary<string, int> RequiredScalars = new Dictionary<string, int>(StringComparer.Ordinal)
    {
        ["AllowDenyLayered"] = 1,
        ["AllowDeviceIDs"] = 1,
        ["AllowDeviceIDsRetroactive"] = 1,
        ["DenyRemovableDevices"] = 1,
    };

    public RegistryPolicyPlan BuildPlan(CompiledPolicy policy, DateTimeOffset nowUtc)
    {
        ArgumentNullException.ThrowIfNull(policy);
        Dictionary<string, string> allowIds = new(StringComparer.OrdinalIgnoreCase);
        foreach (string installId in policy.Entries
                     .Where(entry => entry.Action == EntryAction.Allow && (entry.ExpiresAtUtc is null || entry.ExpiresAtUtc > nowUtc))
                     .SelectMany(entry => entry.DeviceInstallIds)
                     .Distinct(StringComparer.OrdinalIgnoreCase))
        {
            allowIds[$"UsbDlp-{Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(installId)))[..16]}"] = installId;
        }
        return new(RequiredScalars, allowIds, []);
    }

    public RegistryPolicyResult Apply(RegistryPolicyPlan plan, bool dryRun)
    {
        ArgumentNullException.ThrowIfNull(plan);
        if (dryRun || !OperatingSystem.IsWindows())
            return new(true, plan.ScalarValues.Count + plan.AllowDeviceIds.Count, plan.Conflicts);

        List<string> conflicts = [];
        int changed = 0;
        using RegistryKey restrictions = Registry.LocalMachine.CreateSubKey(RestrictionsPath, writable: true);
        using RegistryKey allowIds = Registry.LocalMachine.CreateSubKey(AllowIdsPath, writable: true);
        using RegistryKey ownership = Registry.LocalMachine.CreateSubKey(OwnershipPath, writable: true);
        HashSet<string> ownedScalars = new(ownership.GetValue("Scalars") as string[] ?? [], StringComparer.Ordinal);
        HashSet<string> ownedAllowValues = new(ownership.GetValue("AllowValues") as string[] ?? [], StringComparer.Ordinal);

        foreach ((string name, int desired) in plan.ScalarValues)
        {
            object? existing = restrictions.GetValue(name);
            if (existing is null || ownedScalars.Contains(name))
            {
                if (!Equals(existing, desired)) { restrictions.SetValue(name, desired, RegistryValueKind.DWord); changed++; }
                ownedScalars.Add(name);
            }
            else if (!Equals(existing, desired)) conflicts.Add($"{RestrictionsPath}\\{name}");
        }

        foreach (string stale in ownedAllowValues.Where(name => !plan.AllowDeviceIds.ContainsKey(name)).ToArray())
        {
            allowIds.DeleteValue(stale, throwOnMissingValue: false); ownedAllowValues.Remove(stale); changed++;
        }
        foreach ((string name, string deviceId) in plan.AllowDeviceIds)
        {
            object? existing = allowIds.GetValue(name);
            if (existing is null || ownedAllowValues.Contains(name))
            {
                if (!string.Equals(existing as string, deviceId, StringComparison.OrdinalIgnoreCase)) { allowIds.SetValue(name, deviceId, RegistryValueKind.String); changed++; }
                ownedAllowValues.Add(name);
            }
            else if (!string.Equals(existing as string, deviceId, StringComparison.OrdinalIgnoreCase)) conflicts.Add($"{AllowIdsPath}\\{name}");
        }
        ownership.SetValue("Scalars", ownedScalars.ToArray(), RegistryValueKind.MultiString);
        ownership.SetValue("AllowValues", ownedAllowValues.ToArray(), RegistryValueKind.MultiString);
        return new(false, changed, conflicts);
    }

    public RegistryPolicyResult Clear(bool dryRun)
    {
        if (dryRun || !OperatingSystem.IsWindows()) return new(true, 0, []);
        int changed = 0;
        using RegistryKey? ownership = Registry.LocalMachine.OpenSubKey(OwnershipPath, writable: true);
        if (ownership is null) return new(false, 0, []);
        using RegistryKey? restrictions = Registry.LocalMachine.OpenSubKey(RestrictionsPath, writable: true);
        using RegistryKey? allowIds = Registry.LocalMachine.OpenSubKey(AllowIdsPath, writable: true);
        foreach (string name in ownership.GetValue("Scalars") as string[] ?? []) { restrictions?.DeleteValue(name, false); changed++; }
        foreach (string name in ownership.GetValue("AllowValues") as string[] ?? []) { allowIds?.DeleteValue(name, false); changed++; }
        Registry.LocalMachine.DeleteSubKeyTree(OwnershipPath, throwOnMissingSubKey: false);
        return new(false, changed, []);
    }
}
