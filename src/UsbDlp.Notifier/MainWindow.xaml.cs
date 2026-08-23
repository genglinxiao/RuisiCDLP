using System.IO.Pipes;
using System.IO;
using System.Diagnostics.CodeAnalysis;
using System.Text;
using System.Text.Json;
using System.Resources;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.ComponentModel;
using UsbDlp.Notifications;
using UsbDlp.Policy;

namespace UsbDlp.Notifier;

[SuppressMessage("Design", "CA1001:Types that own disposable fields should be disposable", Justification = "The WPF application calls StopListening during application shutdown.")]
public partial class MainWindow : Window
{
    private const string PipeName = "UsbDlp.Agent.Notifier.v1";
    private readonly CancellationTokenSource _shutdown = new();
    private NotifierMessage? _current;
    private StreamWriter? _writer;
    private bool _connected;
    private bool _submitting;
    private static readonly ResourceManager TextResources = new("UsbDlp.Notifier.Resources.Strings", typeof(MainWindow).Assembly);

    public MainWindow()
    {
        InitializeComponent();
        JustificationBox.TextChanged += JustificationBox_TextChanged;
        ApplyLocalization();
    }

    private static string Text(string key) => TextResources.GetString(key, CultureInfo.CurrentUICulture) ?? key;

    private void ApplyLocalization()
    {
        Title = HeadingText.Text = Text("Title");
        ConnectionText.Text = Text("Connecting");
        BlockedHeadingText.Text = Text("Blocked");
        ReasonText.Text = Text("Waiting");
        DeviceLabel.Text = Text("Device");
        InstanceLabel.Text = Text("Instance");
        IdentifierLabel.Text = Text("Identifier");
        DurationLabel.Text = Text("Duration");
        string[] durations = [Text("Day1"), Text("Day7"), Text("Day30"), Text("Day90")];
        for (int index = 0; index < DurationBox.Items.Count; index++)
            ((ComboBoxItem)DurationBox.Items[index]).Content = durations[index];
        JustificationLabel.Text = Text("Justification");
        JustificationHint.Text = Text("Hint");
        PrivacyText.Text = Text("Privacy");
        SubmitButton.Content = Text("Submit");
    }

    public void StartListening() => _ = ConnectLoopAsync(_shutdown.Token);

    public void StopListening()
    {
        _shutdown.Cancel();
        _shutdown.Dispose();
    }

    private void Window_Closing(object? sender, CancelEventArgs e)
    {
        e.Cancel = true;
        Hide();
        ShowInTaskbar = false;
    }

    private async Task ConnectLoopAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                await using NamedPipeClientStream pipe = new(".", PipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
                await pipe.ConnectAsync(3000, cancellationToken);
                using StreamReader reader = new(pipe, Encoding.UTF8, false, 4096, true);
                using StreamWriter writer = new(pipe, new UTF8Encoding(false), 4096, true) { AutoFlush = true };
                _writer = writer;
                _connected = true;
                ConnectionText.Text = Text("Connected");
                UpdateSubmitState();
                while (!cancellationToken.IsCancellationRequested)
                {
                    string? line = await reader.ReadLineAsync(cancellationToken);
                    if (line is null) break;
                    NotifierMessage? message = JsonSerializer.Deserialize<NotifierMessage>(line, NotifierJson.Options);
                    if (message is not null) ApplyMessage(message);
                }
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { return; }
            catch (Exception exception)
            {
                LogConnectionFailure(exception);
                ConnectionText.Text = Text("Unavailable");
                await Task.Delay(2000, cancellationToken);
            }
            finally
            {
                _connected = false;
                _writer = null;
                UpdateSubmitState();
            }
        }
    }

    private static void LogConnectionFailure(Exception exception)
    {
        try
        {
            string directory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "UsbDlp");
            Directory.CreateDirectory(directory);
            File.AppendAllText(
                Path.Combine(directory, "notifier.log"),
                $"{DateTimeOffset.Now:O} {exception}\n",
                Encoding.UTF8);
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    private void ApplyMessage(NotifierMessage message)
    {
        if (message.Type == "blocked" && message.Device is not null)
        {
            bool isNewRequest = _current?.EventId != message.EventId;
            _current = message;
            DeviceNameText.Text = message.Device.FriendlyName;
            InstanceIdText.Text = message.Device.InstanceId;
            VidPidText.Text = $"VID {message.Device.Vid} · PID {message.Device.Pid}";
            ReasonText.Text = message.ReasonCode?.Replace('_', ' ') ?? Text("Policy");
            if (isNewRequest)
            {
                _submitting = message.Status == "recent";
                DurationBox.SelectedIndex = 2;
                JustificationBox.Clear();
                DurationBox.IsEnabled = !_submitting;
                JustificationBox.IsEnabled = !_submitting;
                StatusText.Foreground = new SolidColorBrush((Color)ColorConverter.ConvertFromString("#3B6D11"));
                StatusText.Text = _submitting ? RecentStatus(message.RetryAfterUtc) : string.Empty;
            }
            UpdateSubmitState();
            ShowInTaskbar = true;
            Show();
            WindowState = WindowState.Normal;
            Activate();
            if (message.Status == "recent")
                _ = DismissAfterAsync(message.EventId, 3500);
        }
        else if (message.Type == "request_result" && _current?.EventId == message.EventId)
        {
            bool failed = message.Status == "error";
            _submitting = !failed;
            StatusText.Foreground = new SolidColorBrush((Color)ColorConverter.ConvertFromString(failed ? "#A32D2D" : "#3B6D11"));
            StatusText.Text = message.Status switch
            {
                "error" => Text("Failed"),
                "recent" => RecentStatus(message.RetryAfterUtc),
                _ => Text("Submitted"),
            };
            UpdateSubmitState();
            if (!failed)
                _ = DismissAfterAsync(message.EventId, message.Status == "recent" ? 3500 : 1500);
        }
        else if (message.Type == "allowed" && message.Device is not null &&
                 string.Equals(_current?.Device?.InstanceId, message.Device.InstanceId, StringComparison.OrdinalIgnoreCase))
        {
            _current = null;
            _submitting = false;
            DurationBox.IsEnabled = true;
            JustificationBox.IsEnabled = true;
            JustificationBox.Clear();
            StatusText.Text = string.Empty;
            UpdateSubmitState();
            Hide();
            ShowInTaskbar = false;
        }
    }

    private void JustificationBox_TextChanged(object sender, TextChangedEventArgs e) => UpdateSubmitState();

    private static string RecentStatus(DateTimeOffset? retryAfterUtc) => Text("Recent").Replace(
        "{0}",
        retryAfterUtc?.ToLocalTime().ToString("t", CultureInfo.CurrentCulture) ?? Text("Soon"),
        StringComparison.Ordinal);

    private async Task DismissAfterAsync(Guid eventId, int delayMilliseconds)
    {
        await Task.Delay(delayMilliseconds);
        if (_current?.EventId != eventId) return;
        _current = null;
        _submitting = false;
        DurationBox.IsEnabled = true;
        JustificationBox.IsEnabled = true;
        JustificationBox.Clear();
        StatusText.Text = string.Empty;
        UpdateSubmitState();
        Hide();
        ShowInTaskbar = false;
    }

    private void UpdateSubmitState()
    {
        if (!IsInitialized) return;
        int length = JustificationBox.Text.Trim().Length;
        SubmitButton.IsEnabled = _connected && !_submitting && _current?.Device is not null && length is >= 10 and <= 2000;
    }

    private async void SubmitButton_Click(object sender, RoutedEventArgs e)
    {
        if (_current?.Device is null || _writer is null) return;
        string justification = JustificationBox.Text.Trim();
        if (justification.Length is < 10 or > 2000)
        {
            StatusText.Foreground = new SolidColorBrush((Color)ColorConverter.ConvertFromString("#A32D2D"));
            StatusText.Text = Text("Invalid");
            return;
        }
        int days = int.Parse(((ComboBoxItem)DurationBox.SelectedItem).Tag.ToString()!, System.Globalization.CultureInfo.InvariantCulture);
        _submitting = true;
        UpdateSubmitState();
        StatusText.Foreground = new SolidColorBrush((Color)ColorConverter.ConvertFromString("#5F5E5A"));
        StatusText.Text = Text("Submitting");
        NotifierAccessRequest request = new(1, "request_access", _current.EventId, _current.Device, days, justification);
        try
        {
            await _writer.WriteLineAsync(JsonSerializer.Serialize(request, NotifierJson.Options));
        }
        catch (Exception exception) when (exception is IOException or ObjectDisposedException)
        {
            _submitting = false;
            StatusText.Foreground = new SolidColorBrush((Color)ColorConverter.ConvertFromString("#A32D2D"));
            StatusText.Text = Text("Failed");
            UpdateSubmitState();
            LogConnectionFailure(exception);
        }
    }
}
