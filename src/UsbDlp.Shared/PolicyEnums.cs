namespace UsbDlp.Policy;

public enum MatchType
{
    InstanceId,
    VidPidSerial,
    VidPid,
    DeviceClass,
}

public enum EntryAction
{
    Allow,
    Block,
}

public enum GroupMode
{
    DefaultDeny,
    DefaultAllow,
}

public enum ScopeType
{
    Global,
    Group,
    Workstation,
}

public enum Decision
{
    Allow,
    Block,
}

public enum ProductDeviceClass
{
    UsbStorage,
    Wpd,
}

