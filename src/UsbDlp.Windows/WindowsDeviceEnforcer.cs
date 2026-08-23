using System.Runtime.InteropServices;
using UsbDlp.Policy;

namespace UsbDlp.Windows;

public sealed record EnforcementResult(bool Changed, bool DryRun, string Operation, string? ErrorCode);

public interface IWindowsDeviceEnforcer
{
    EnforcementResult Apply(DeviceInfo device, Decision decision, bool dryRun);
    bool Owns(DeviceInfo device);
    void Forget(DeviceInfo device);
}

public sealed class WindowsDeviceEnforcer : IWindowsDeviceEnforcer
{
    private const uint CrSuccess = 0;
    private const uint DisableUiNotOk = 0x00000002;
    private const uint DeviceNodeHasProblem = 0x00000400;
    private const uint ProblemDisabled = 0x00000016;
    private readonly HashSet<string> _ownedDisabledIds = new(StringComparer.OrdinalIgnoreCase);
    private readonly object _sync = new();

    public bool Owns(DeviceInfo device)
    {
        ArgumentNullException.ThrowIfNull(device);
        lock (_sync)
            return _ownedDisabledIds.Contains(EnforcementTarget(device));
    }

    public void Forget(DeviceInfo device)
    {
        ArgumentNullException.ThrowIfNull(device);
        lock (_sync)
            _ownedDisabledIds.Remove(EnforcementTarget(device));
    }

    public EnforcementResult Apply(DeviceInfo device, Decision decision, bool dryRun)
    {
        ArgumentNullException.ThrowIfNull(device);
        if (HardCodedSafetyAllowList.Contains(device.ClassGuid) || HardCodedSafetyAllowList.ContainsAny(device.AncestryClassGuids))
        {
            return new(false, dryRun, "safety_allow", null);
        }

        lock (_sync)
        {
            string targetInstanceId = EnforcementTarget(device);
            if (decision == Decision.Block)
            {
                if (_ownedDisabledIds.Contains(targetInstanceId) && IsDisabled(targetInstanceId))
                    return new(false, dryRun, "already_disabled", null);
                if (dryRun)
                    return new(false, true, "would_disable", null);
                uint result = LocateAndDisable(targetInstanceId);
                if (result == CrSuccess)
                {
                    _ownedDisabledIds.Add(targetInstanceId);
                    return new(true, false, "disabled", null);
                }
                return new(false, false, "disable_failed", $"CM_{result:X8}");
            }

            if (!_ownedDisabledIds.Contains(targetInstanceId))
            {
                uint refresh = dryRun ? CrSuccess : RefreshDeviceTree(targetInstanceId);
                return new(false, dryRun, refresh == CrSuccess ? "refreshed" : "not_owned", null);
            }
            if (dryRun)
                return new(false, true, "would_enable", null);
            uint enableResult = LocateEnableAndRefresh(targetInstanceId);
            if (enableResult == CrSuccess)
            {
                _ownedDisabledIds.Remove(targetInstanceId);
                return new(true, false, "enabled", null);
            }
            return new(false, false, "enable_failed", $"CM_{enableResult:X8}");
        }
    }

    internal static string EnforcementTarget(DeviceInfo device) =>
        device.DeviceInstallIds.FirstOrDefault(id => id.StartsWith("USBSTOR\\", StringComparison.OrdinalIgnoreCase)) ??
        (device.DeviceClass == ProductDeviceClass.Wpd
            ? device.DeviceInstallIds.FirstOrDefault(id => !id.StartsWith("USB\\VID_", StringComparison.OrdinalIgnoreCase))
            : null) ??
        device.InstanceId;

    private static uint LocateAndDisable(string instanceId) =>
        NativeMethods.CM_Locate_DevNodeW(out uint node, instanceId, 0) is uint locate && locate != CrSuccess
            ? locate
            : NativeMethods.CM_Disable_DevNode(node, DisableUiNotOk);

    private static bool IsDisabled(string instanceId)
    {
        uint locate = NativeMethods.CM_Locate_DevNodeW(out uint node, instanceId, 0);
        if (locate != CrSuccess)
            return false;

        uint statusResult = NativeMethods.CM_Get_DevNode_Status(out uint status, out uint problem, node, 0);
        return statusResult == CrSuccess &&
               (status & DeviceNodeHasProblem) != 0 &&
               problem == ProblemDisabled;
    }

    private static uint LocateEnableAndRefresh(string instanceId)
    {
        uint locate = NativeMethods.CM_Locate_DevNodeW(out uint node, instanceId, 0);
        if (locate != CrSuccess) return locate;
        uint enable = NativeMethods.CM_Enable_DevNode(node, 0);
        return enable == CrSuccess ? RefreshDeviceTree(node) : enable;
    }

    private static uint RefreshDeviceTree(string instanceId)
    {
        uint locate = NativeMethods.CM_Locate_DevNodeW(out uint node, instanceId, 0);
        return locate == CrSuccess ? RefreshDeviceTree(node) : locate;
    }

    private static uint RefreshDeviceTree(uint node)
    {
        uint refreshNode = NativeMethods.CM_Get_Parent(out uint parent, node, 0) == CrSuccess ? parent : node;
        return NativeMethods.CM_Reenumerate_DevNode(refreshNode, 0);
    }

    private static class NativeMethods
    {
        [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
        internal static extern uint CM_Locate_DevNodeW(out uint devNode, string deviceId, uint flags);
        [DllImport("cfgmgr32.dll")]
        internal static extern uint CM_Disable_DevNode(uint devNode, uint flags);
        [DllImport("cfgmgr32.dll")]
        internal static extern uint CM_Enable_DevNode(uint devNode, uint flags);
        [DllImport("cfgmgr32.dll")]
        internal static extern uint CM_Get_DevNode_Status(out uint status, out uint problemNumber, uint devInst, uint flags);
        [DllImport("cfgmgr32.dll")]
        internal static extern uint CM_Get_Parent(out uint parentDevInst, uint devInst, uint flags);
        [DllImport("cfgmgr32.dll")]
        internal static extern uint CM_Reenumerate_DevNode(uint devInst, uint flags);
    }
}
