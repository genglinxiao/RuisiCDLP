using System.ComponentModel;
using System.Diagnostics;
using System.Diagnostics.CodeAnalysis;
using System.Runtime.InteropServices;
using System.Text;

namespace UsbDlp.Agent;

public sealed partial class InteractiveNotifierLauncher(AgentOptions options, ILogger<InteractiveNotifierLauncher> logger)
{
    private const uint MaximumAllowed = 0x02000000;
    private const uint CreateUnicodeEnvironment = 0x00000400;
    private readonly ILogger<InteractiveNotifierLauncher> _logger = logger;

    public void EnsureStarted()
    {
        if (!OperatingSystem.IsWindows()) return;
        string? executable = options.NotifierExecutablePath;
        if (string.IsNullOrWhiteSpace(executable) || !File.Exists(executable))
        {
            LogUnavailable(executable ?? "not configured");
            return;
        }

        List<uint> sessionIds;
        try
        {
            sessionIds = ActiveSessionIds();
        }
        catch (Win32Exception exception)
        {
            LogFailure(0, exception.Message);
            return;
        }

        foreach (uint sessionId in sessionIds)
        {
            try
            {
                if (IsRunningInSession(executable, sessionId)) continue;
                if ((uint)Process.GetCurrentProcess().SessionId == sessionId && Environment.UserInteractive)
                {
                    Process.Start(new ProcessStartInfo(executable)
                    {
                        UseShellExecute = false,
                        WorkingDirectory = Path.GetDirectoryName(executable) ?? AppContext.BaseDirectory
                    });
                }
                else
                {
                    StartAsSessionUser(executable, sessionId);
                }
                LogStarted(sessionId);
            }
            catch (Exception exception) when (exception is Win32Exception or InvalidOperationException)
            {
                LogFailure(sessionId, exception.Message);
            }
        }
    }

    private static bool IsRunningInSession(string executable, uint sessionId)
    {
        string processName = Path.GetFileNameWithoutExtension(executable);
        foreach (Process process in Process.GetProcessesByName(processName))
        {
            using (process)
            {
                try
                {
                    if ((uint)process.SessionId == sessionId) return true;
                }
                catch (InvalidOperationException) { }
            }
        }
        return false;
    }

    private static List<uint> ActiveSessionIds()
    {
        if (!NativeMethods.WTSEnumerateSessionsW(IntPtr.Zero, 0, 1, out IntPtr sessions, out int count))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        try
        {
            List<uint> result = [];
            int size = Marshal.SizeOf<WtsSessionInfo>();
            for (int index = 0; index < count; index++)
            {
                WtsSessionInfo item = Marshal.PtrToStructure<WtsSessionInfo>(IntPtr.Add(sessions, index * size));
                if (item.State == WtsConnectState.Active) result.Add(item.SessionId);
            }
            return result;
        }
        finally { NativeMethods.WTSFreeMemory(sessions); }
    }

    private static void StartAsSessionUser(string executable, uint sessionId)
    {
        if (!NativeMethods.WTSQueryUserToken(sessionId, out IntPtr impersonationToken))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        try
        {
            if (!NativeMethods.DuplicateTokenEx(impersonationToken, MaximumAllowed, IntPtr.Zero, 2, 1, out IntPtr primaryToken))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            try
            {
                if (!NativeMethods.CreateEnvironmentBlock(out IntPtr environment, primaryToken, false))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                try
                {
                    StartupInfo startup = new() { Size = Marshal.SizeOf<StartupInfo>(), Desktop = @"winsta0\default" };
                    StringBuilder commandLine = new($"\"{executable}\"");
                    if (!NativeMethods.CreateProcessAsUserW(
                            primaryToken, executable, commandLine, IntPtr.Zero, IntPtr.Zero, false,
                            CreateUnicodeEnvironment, environment, Path.GetDirectoryName(executable), ref startup, out ProcessInformation process))
                        throw new Win32Exception(Marshal.GetLastWin32Error());
                    NativeMethods.CloseHandle(process.Thread);
                    NativeMethods.CloseHandle(process.Process);
                }
                finally { NativeMethods.DestroyEnvironmentBlock(environment); }
            }
            finally { NativeMethods.CloseHandle(primaryToken); }
        }
        finally { NativeMethods.CloseHandle(impersonationToken); }
    }

    [LoggerMessage(EventId = 1210, Level = LogLevel.Information, Message = "Notifier started in session {SessionId}")]
    private partial void LogStarted(uint sessionId);
    [LoggerMessage(EventId = 1211, Level = LogLevel.Warning, Message = "Notifier executable is unavailable: {Path}")]
    private partial void LogUnavailable(string path);
    [LoggerMessage(EventId = 1212, Level = LogLevel.Warning, Message = "Could not start notifier in session {SessionId}: {Error}")]
    private partial void LogFailure(uint sessionId, string error);

    private enum WtsConnectState { Active, Connected, ConnectQuery, Shadow, Disconnected, Idle, Listen, Reset, Down, Init }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WtsSessionInfo
    {
        public uint SessionId;
        public IntPtr WinStationName;
        public WtsConnectState State;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct StartupInfo
    {
        public int Size;
        public string? Reserved;
        public string? Desktop;
        public string? Title;
        public int X;
        public int Y;
        public int XSize;
        public int YSize;
        public int XCountChars;
        public int YCountChars;
        public int FillAttribute;
        public int Flags;
        public short ShowWindow;
        public short Reserved2;
        public IntPtr ReservedPointer;
        public IntPtr StandardInput;
        public IntPtr StandardOutput;
        public IntPtr StandardError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessInformation
    {
        public IntPtr Process;
        public IntPtr Thread;
        public uint ProcessId;
        public uint ThreadId;
    }

    private static class NativeMethods
    {
        [DllImport("wtsapi32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool WTSEnumerateSessionsW(IntPtr server, int reserved, int version, out IntPtr sessions, out int count);
        [DllImport("wtsapi32.dll")]
        internal static extern void WTSFreeMemory(IntPtr memory);
        [DllImport("wtsapi32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool WTSQueryUserToken(uint sessionId, out IntPtr token);
        [DllImport("advapi32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool DuplicateTokenEx(IntPtr existingToken, uint desiredAccess, IntPtr attributes, int impersonationLevel, int tokenType, out IntPtr newToken);
        [DllImport("userenv.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool CreateEnvironmentBlock(out IntPtr environment, IntPtr token, [MarshalAs(UnmanagedType.Bool)] bool inherit);
        [DllImport("userenv.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool DestroyEnvironmentBlock(IntPtr environment);
        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        [SuppressMessage("Performance", "CA1838:Avoid StringBuilder parameters for P/Invokes", Justification = "CreateProcessAsUser requires a mutable command-line buffer.")]
        internal static extern bool CreateProcessAsUserW(IntPtr token, string applicationName, StringBuilder commandLine, IntPtr processAttributes, IntPtr threadAttributes, [MarshalAs(UnmanagedType.Bool)] bool inheritHandles, uint creationFlags, IntPtr environment, string? currentDirectory, ref StartupInfo startupInfo, out ProcessInformation processInformation);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool CloseHandle(IntPtr handle);
    }
}
