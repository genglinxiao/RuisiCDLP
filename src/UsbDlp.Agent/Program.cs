using UsbDlp.Agent;
using UsbDlp.Windows;

HostApplicationBuilder builder = Host.CreateApplicationBuilder(args);
builder.Services.AddWindowsService(serviceOptions => serviceOptions.ServiceName = "UsbDlp Agent");
AgentOptions options = builder.Configuration.GetSection(AgentOptions.SectionName).Get<AgentOptions>() ?? new AgentOptions();
builder.Services.AddSingleton(options);
builder.Services.AddSingleton<AgentStorage>();
builder.Services.AddSingleton<DurableEventStore>();
builder.Services.AddSingleton<AgentApiClient>();
builder.Services.AddSingleton<WindowsDeviceEnumerator>();
builder.Services.AddSingleton<IWindowsDeviceEnumerator, ConfiguredDeviceEnumerator>();
builder.Services.AddSingleton<IWindowsDeviceEnforcer, WindowsDeviceEnforcer>();
builder.Services.AddSingleton<IRegistryPolicyManager, RegistryPolicyManager>();
builder.Services.AddSingleton<InteractiveNotifierLauncher>();
builder.Services.AddSingleton<NotifierPipeServer>();
builder.Services.AddHostedService(serviceProvider => serviceProvider.GetRequiredService<NotifierPipeServer>());
builder.Services.AddHostedService<Worker>();

await builder.Build().RunAsync();
