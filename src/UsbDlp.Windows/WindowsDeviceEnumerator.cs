using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using UsbDlp.Policy;

namespace UsbDlp.Windows;

public interface IWindowsDeviceEnumerator
{
    IReadOnlyList<DeviceInfo> EnumerateInScopeDevices();
}

public sealed class WindowsDeviceEnumerator : IWindowsDeviceEnumerator
{
    private const uint CrSuccess = 0;
    private const uint GetIdListPresent = 0x00000100;
    private const uint RegPropertyHardwareId = 2;
    private const uint RegPropertyCompatibleIds = 3;
    private const uint RegPropertyService = 5;
    private const uint RegPropertyClassGuid = 9;
    private const uint RegPropertyManufacturer = 12;
    private const uint RegPropertyFriendlyName = 13;
    private const uint RegPropertyDeviceDescription = 1;
    private const string WpdClassGuid = "{eec5ad98-8080-425f-922a-dabf3de3f69a}";

    public IReadOnlyList<DeviceInfo> EnumerateInScopeDevices()
    {
        if (!OperatingSystem.IsWindows())
        {
            return [];
        }

        List<DeviceInfo> devices = [];
        Dictionary<string, int> deviceIndexes = new(StringComparer.OrdinalIgnoreCase);
        foreach (string instanceId in EnumeratePresentIds())
        {
            if (NativeMethods.CM_Locate_DevNodeW(out uint devNode, instanceId, 0) != CrSuccess)
            {
                continue;
            }

            string? service = ReadStringProperty(devNode, RegPropertyService);
            string? classGuid = ReadStringProperty(devNode, RegPropertyClassGuid);
            bool storage = string.Equals(service, "USBSTOR", StringComparison.OrdinalIgnoreCase) ||
                instanceId.StartsWith("USBSTOR\\", StringComparison.OrdinalIgnoreCase);
            bool wpd = string.Equals(classGuid, WpdClassGuid, StringComparison.OrdinalIgnoreCase);
            if (!storage && !wpd)
            {
                continue;
            }

            (uint CanonicalNode, string CanonicalId, IReadOnlyList<string> AncestryGuids)? canonical = ResolveCanonicalUsbAncestor(devNode);
            if (canonical is null)
            {
                continue;
            }

            if (deviceIndexes.TryGetValue(canonical.Value.CanonicalId, out int existingIndex) &&
                (devices[existingIndex].DeviceClass == ProductDeviceClass.UsbStorage || !storage))
            {
                continue;
            }

            ParsedUsbIdentity? identity = DeviceIdentityParser.Parse(canonical.Value.CanonicalId);
            if (identity is null || HardCodedSafetyAllowList.ContainsAny(canonical.Value.AncestryGuids))
            {
                continue;
            }

            IReadOnlyList<string> canonicalHardwareIds = ReadMultiStringProperty(canonical.Value.CanonicalNode, RegPropertyHardwareId);
            IReadOnlyList<string> governedHardwareIds = ReadMultiStringProperty(devNode, RegPropertyHardwareId);
            IReadOnlyList<string> canonicalCompatibleIds = ReadMultiStringProperty(canonical.Value.CanonicalNode, RegPropertyCompatibleIds);
            IReadOnlyList<string> governedCompatibleIds = ReadMultiStringProperty(devNode, RegPropertyCompatibleIds);
            string friendlyName = ReadStringProperty(devNode, RegPropertyFriendlyName) ??
                ReadStringProperty(canonical.Value.CanonicalNode, RegPropertyFriendlyName) ?? canonical.Value.CanonicalId;
            string? manufacturer = ReadStringProperty(canonical.Value.CanonicalNode, RegPropertyManufacturer) ??
                ReadStringProperty(devNode, RegPropertyManufacturer);
            string? description = ReadStringProperty(devNode, RegPropertyDeviceDescription) ??
                ReadStringProperty(canonical.Value.CanonicalNode, RegPropertyDeviceDescription);
            string? model = ReadBusReportedDescription(canonical.Value.CanonicalNode) ??
                ReadBusReportedDescription(devNode);
            // Keep the governed child first so enforcement does not disable unrelated
            // functions exposed by the same composite USB peripheral.
            List<string> installIds = [
                instanceId,
                .. governedHardwareIds,
                .. governedCompatibleIds,
                canonical.Value.CanonicalId,
                .. canonicalHardwareIds,
                .. canonicalCompatibleIds,
            ];
            DeviceInfo candidate = new(
                canonical.Value.CanonicalId,
                canonicalHardwareIds,
                installIds.Distinct(StringComparer.OrdinalIgnoreCase).Take(64).ToArray(),
                identity.Vid,
                identity.Pid,
                identity.Serial,
                storage ? ProductDeviceClass.UsbStorage : ProductDeviceClass.Wpd,
                classGuid ?? string.Empty,
                service,
                friendlyName,
                "usb",
                storage ? ["usb_storage"] : ["mtp"],
                canonical.Value.AncestryGuids,
                manufacturer,
                model,
                description);
            if (deviceIndexes.TryGetValue(canonical.Value.CanonicalId, out existingIndex))
            {
                // A storage node governs the whole disk. Prefer it over a WPD volume child so
                // every partition is removed when a multi-volume USB device is blocked.
                devices[existingIndex] = candidate;
            }
            else
            {
                deviceIndexes[canonical.Value.CanonicalId] = devices.Count;
                devices.Add(candidate);
            }
        }

        return devices;
    }

    private static IEnumerable<string> EnumeratePresentIds()
    {
        uint result = NativeMethods.CM_Get_Device_ID_List_SizeW(out uint characters, null, GetIdListPresent);
        if (result != CrSuccess || characters == 0)
        {
            yield break;
        }

        char[] buffer = new char[characters];
        result = NativeMethods.CM_Get_Device_ID_ListW(null, buffer, characters, GetIdListPresent);
        if (result != CrSuccess)
        {
            throw new Win32Exception((int)result, "Could not enumerate present Plug and Play devices.");
        }

        string all = new(buffer);
        foreach (string id in all.Split('\0', StringSplitOptions.RemoveEmptyEntries))
        {
            yield return id;
        }
    }

    private static (uint CanonicalNode, string CanonicalId, IReadOnlyList<string> AncestryGuids)? ResolveCanonicalUsbAncestor(uint startNode)
    {
        uint current = startNode;
        List<string> ancestry = [];
        for (int depth = 0; depth < 16; depth++)
        {
            string id = GetDeviceId(current);
            if (id.StartsWith("USB\\VID_", StringComparison.OrdinalIgnoreCase))
            {
                return (current, id, ancestry);
            }

            string? classGuid = ReadStringProperty(current, RegPropertyClassGuid);
            if (classGuid is not null)
            {
                ancestry.Add(classGuid);
            }

            if (NativeMethods.CM_Get_Parent(out uint parent, current, 0) != CrSuccess)
            {
                return null;
            }
            current = parent;
        }

        return null;
    }

    private static string GetDeviceId(uint devNode)
    {
        char[] buffer = new char[201];
        uint result = NativeMethods.CM_Get_Device_IDW(devNode, buffer, buffer.Length, 0);
        return result == CrSuccess ? new string(buffer).TrimEnd('\0') : string.Empty;
    }

    private static string? ReadStringProperty(uint devNode, uint property)
    {
        byte[] bytes = ReadProperty(devNode, property);
        return bytes.Length < 2 ? null : Encoding.Unicode.GetString(bytes).TrimEnd('\0');
    }

    private static string[] ReadMultiStringProperty(uint devNode, uint property)
    {
        byte[] bytes = ReadProperty(devNode, property);
        return bytes.Length < 2
            ? []
            : Encoding.Unicode.GetString(bytes).Split('\0', StringSplitOptions.RemoveEmptyEntries);
    }

    private static byte[] ReadProperty(uint devNode, uint property)
    {
        uint length = 0;
        _ = NativeMethods.CM_Get_DevNode_Registry_PropertyW(devNode, property, out _, null, ref length, 0);
        if (length == 0 || length > 64 * 1024)
        {
            return [];
        }

        byte[] buffer = new byte[length];
        uint result = NativeMethods.CM_Get_DevNode_Registry_PropertyW(devNode, property, out _, buffer, ref length, 0);
        return result == CrSuccess ? buffer[..(int)length] : [];
    }

    private static string? ReadBusReportedDescription(uint devNode)
    {
        DevPropKey key = new(new Guid("540b947e-8b40-45bc-a8a2-6a0b894cbda2"), 4);
        uint length = 0;
        _ = NativeMethods.CM_Get_DevNode_PropertyW(devNode, ref key, out _, null, ref length, 0);
        if (length < 2 || length > 4096)
        {
            return null;
        }

        byte[] buffer = new byte[length];
        uint result = NativeMethods.CM_Get_DevNode_PropertyW(devNode, ref key, out _, buffer, ref length, 0);
        return result == CrSuccess ? Encoding.Unicode.GetString(buffer, 0, (int)length).TrimEnd('\0') : null;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DevPropKey(Guid formatId, uint propertyId)
    {
        public Guid FormatId = formatId;
        public uint PropertyId = propertyId;
    }

    private static class NativeMethods
    {
        [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
        internal static extern uint CM_Get_Device_ID_List_SizeW(out uint length, string? filter, uint flags);

        [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
        internal static extern uint CM_Get_Device_ID_ListW(string? filter, [Out] char[] buffer, uint bufferLength, uint flags);

        [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
        internal static extern uint CM_Locate_DevNodeW(out uint devNode, string deviceId, uint flags);

        [DllImport("cfgmgr32.dll")]
        internal static extern uint CM_Get_Parent(out uint parent, uint devNode, uint flags);

        [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
        internal static extern uint CM_Get_Device_IDW(uint devNode, [Out] char[] buffer, int bufferLength, uint flags);

        [DllImport("cfgmgr32.dll")]
        internal static extern uint CM_Get_DevNode_Registry_PropertyW(uint devNode, uint property, out uint registryType, [Out] byte[]? buffer, ref uint bufferLength, uint flags);

        [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
        internal static extern uint CM_Get_DevNode_PropertyW(uint devNode, ref DevPropKey propertyKey, out uint propertyType, [Out] byte[]? buffer, ref uint bufferLength, uint flags);
    }
}
