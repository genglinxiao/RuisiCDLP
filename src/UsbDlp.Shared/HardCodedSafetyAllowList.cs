namespace UsbDlp.Policy;

public static class HardCodedSafetyAllowList
{
    public const string HidClassGuid = "{745a17a0-74d3-11d0-b6fe-00a0c90f57da}";
    public const string KeyboardClassGuid = "{4d36e96b-e325-11ce-bfc1-08002be10318}";
    public const string MouseClassGuid = "{4d36e96f-e325-11ce-bfc1-08002be10318}";
    public const string SmartCardReaderClassGuid = "{50dd5230-ba8a-11d1-bf5d-0000f805f530}";
    public const string PrinterClassGuid = "{4d36e979-e325-11ce-bfc1-08002be10318}";
    public const string UsbControllerAndHubClassGuid = "{36fc9e60-c465-11cf-8056-444553540000}";
    public const string MediaClassGuid = "{4d36e96c-e325-11ce-bfc1-08002be10318}";
    public const string AudioEndpointClassGuid = "{c166523c-fe0c-4a94-a586-f1a80cfbbf3e}";
    public const string BluetoothClassGuid = "{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}";
    public const string NetworkClassGuid = "{4d36e972-e325-11ce-bfc1-08002be10318}";
    public const string SystemClassGuid = "{4d36e97d-e325-11ce-bfc1-08002be10318}";
    public const string ImageClassGuid = "{6bdd1fc6-810f-11d0-bec7-08002be2092f}";
    public const string CameraClassGuid = "{ca3e7ab9-b4c3-4ae6-8251-579ef933890f}";

    private static readonly HashSet<string> SafetyClassGuids = new(
        [
            HidClassGuid,
            KeyboardClassGuid,
            MouseClassGuid,
            SmartCardReaderClassGuid,
            PrinterClassGuid,
            UsbControllerAndHubClassGuid,
            MediaClassGuid,
            AudioEndpointClassGuid,
            BluetoothClassGuid,
            NetworkClassGuid,
            SystemClassGuid,
            ImageClassGuid,
            CameraClassGuid,
        ],
        StringComparer.OrdinalIgnoreCase);

    public static bool Contains(string classGuid) =>
        !string.IsNullOrWhiteSpace(classGuid) && SafetyClassGuids.Contains(classGuid);

    public static bool ContainsAny(IEnumerable<string> classGuids)
    {
        ArgumentNullException.ThrowIfNull(classGuids);
        return classGuids.Any(Contains);
    }
}

