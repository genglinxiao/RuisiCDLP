using System.Configuration;
using System.Data;
using System.Windows;

namespace UsbDlp.Notifier;

[System.Diagnostics.CodeAnalysis.SuppressMessage("Design", "CA1001:Types that own disposable fields should be disposable", Justification = "WPF owns the application lifetime; OnExit releases and disposes the session mutex.")]
public partial class App : Application
{
    private System.Threading.Mutex? _singleInstance;
    private bool _ownsMutex;
    private MainWindow? _window;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        string mutexName = $@"Local\UsbDlp.Notifier.{System.Diagnostics.Process.GetCurrentProcess().SessionId}";
        _singleInstance = new System.Threading.Mutex(initiallyOwned: true, mutexName, out bool createdNew);
        if (!createdNew)
        {
            Shutdown();
            return;
        }
        _ownsMutex = true;

        ShutdownMode = ShutdownMode.OnExplicitShutdown;
        _window = new MainWindow();
        MainWindow = _window;
        _window.StartListening();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _window?.StopListening();
        if (_ownsMutex) _singleInstance?.ReleaseMutex();
        _singleInstance?.Dispose();
        base.OnExit(e);
    }
}

