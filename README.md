# UsbDlp

Windows-first USB device control with a central ASP.NET Core administration server, a Windows
service agent, a per-user WPF notifier, and a safety-interlocked diagnostic CLI. The normative
contracts are in `usb_control_specs`.

> Compatibility status: native Windows XP SP3 x86, Windows 7 SP1 x86/x64, and Windows 10
> x86/x64 agent/server binaries and restricted deployment kits can be built from `legacy/`.
> Native tests pass on the Windows 11 build host, but the packages remain `UNVERIFIED` until the
> isolated VM and hardware qualification matrix in the test plan is complete. Do not use the
> modern x64 MSI on XP or Windows 7.

## Fast end-to-end demo

Prerequisites: Windows 10/11 and the .NET 8 SDK. From an ordinary PowerShell prompt:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\start-demo.ps1 `
  -AdminPassword "Choose-a-long-demo-password!"
```

This builds everything, starts the server at `http://127.0.0.1:5091`, enrolls an agent, launches
the notifier, and injects the sample SanDisk device. Enforcement is always dry-run in this script;
no hardware or registry policy is changed. Sign in as `admin`, then inspect the workstation and
blocked event. Submit a 10+ character justification in the notifier, approve it on **Requests**,
and wait about five seconds for the workstation policy version to converge. Use **EN | 中文** to
switch the administration UI language.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\stop-demo.ps1
```

Logs and the disposable SQLite database remain in `.run\demo`. Add `-Reset` on the next start to
discard them. Use `-UsePhysicalDevices` to inventory attached hardware while retaining dry-run.
After safely ejecting test media, an elevated prompt may add `-EnableLiveEnforcement`; this can
forcibly disable mounted devices and must never be used while writes are in progress.

## Run the server separately

Generate a deployment token and start the server in the foreground:

```powershell
$token = ([guid]::NewGuid().ToString("N") + [guid]::NewGuid().ToString("N"))
powershell -ExecutionPolicy Bypass -File scripts\start-server.ps1 `
  -AdminPassword "Choose-a-long-admin-password!" `
  -EnrollmentToken $token
```

For a production-like deployment, terminate TLS at ASP.NET Core or a trusted reverse proxy, set
the `UsbDlp__*` settings in the service environment, protect the SQLite path, and rotate the
bootstrap enrollment token after rollout. HTTP is provided only for the loopback demo.

## Build and install the server MSI

The server has its own x64 package. It does not contain the workstation agent, notifier, or CLI,
and the workstation MSI does not contain the server:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-server-installer.ps1
```

Output: `artifacts\installer\RuisiUsbDlpServer-1.1.0-x64.msi`. Create separate ACL-protected
files containing a 14+ character administrator password and a 32+ character enrollment token,
then install from an elevated prompt:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install-server-package.ps1 `
  -AdminPasswordFile "C:\SecureStaging\admin-password.txt" `
  -EnrollmentTokenFile "C:\SecureStaging\enrollment-token.txt"
```

This installs the auto-start `UsbDlp.Server` Windows service and listens only on
`http://127.0.0.1:5091`. Server state is ACL-protected under
`%ProgramData%\Ruisi\UsbDlpServer`. For an isolated LAN test, explicitly select a non-loopback
listener and firewall access; production deployments should use HTTPS instead:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install-server-package.ps1 `
  -AdminPasswordFile "C:\SecureStaging\admin-password.txt" `
  -EnrollmentTokenFile "C:\SecureStaging\enrollment-token.txt" `
  -ListenUrl "http://0.0.0.0:5091" -ExposeToLan -AllowInsecureHttp
```

Uninstall preserves the database by default. `-PurgeData` permanently removes server state:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\uninstall-server.ps1
powershell -ExecutionPolicy Bypass -File scripts\uninstall-server.ps1 -PurgeData
```

The administration console treats **Devices** and **Workstations** as searchable inventory.
Device detail shows every workstation where the device was observed; workstation detail shows
every observed USB device. Each pair reports current effective policy separately from the latest
observed enforcement and freshness. Administrators can add or revoke a direct pair grant without
searching historical requests. Existing request/event JSON is backfilled into inventory at startup.

## Build the workstation MSI

The x64 WiX 4 package is self-contained and includes the agent service, on-demand notifier,
diagnostic CLI, .NET 8 runtime, and native SQLite runtime:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-agent-installer.ps1
```

Output: `artifacts\installer\RuisiUsbDlpAgent-1.1.0-x64.msi`. The build prints its SHA-256 hash.
The development artifact is not Authenticode-signed; sign both binaries and MSI with the
organization's code-signing certificate before managed production deployment.

## Install a workstation

Put the server enrollment token in an ACL-protected temporary file, then run an elevated prompt:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install-workstation.ps1 `
  -ServerUrl "https://usb-control.example.com" `
  -EnrollmentTokenFile "C:\SecureStaging\usb-enrollment-token.txt"
```

The MSI installs the self-contained payload to `%ProgramFiles%\Ruisi USB DLP`, stores state in
`%ProgramData%\UsbDlp`, and installs an auto-start/recovery LocalSystem service. The notifier is
launched on demand by the agent and never displays an idle waiting window. The token copy is
restricted to SYSTEM and Administrators and deleted after successful enrollment. Dry-run is the
default. Enable real device and registry mutation only after validating inventory:

```powershell
dotnet run --project src\UsbDlp.Cli -- monitor
powershell -ExecutionPolicy Bypass -File scripts\install-workstation.ps1 `
  -ServerUrl "https://usb-control.example.com" `
  -EnrollmentTokenFile "C:\SecureStaging\usb-enrollment-token.txt" `
  -EnableLiveEnforcement
```

For a same-machine HTTP lab only, use `-ServerUrl http://127.0.0.1:5091 -AllowLocalHttp`.

For direct enterprise deployment, use equivalent MSI properties without placing the token itself
on the command line:

```powershell
msiexec /i RuisiUsbDlpAgent-1.1.0-x64.msi /qn /norestart `
  SERVERURL="https://usb-control.example.com" `
  ENROLLMENTTOKENFILE="C:\SecureStaging\usb-enrollment-token.txt" `
  DRYRUN=true ALLOWINSECUREHTTP=false LOCALPOLLSECONDS=2
```

Uninstall from an elevated prompt. This removes the service, binaries, notifier launch support,
and only UsbDlp-owned Device Installation Restriction values. State is retained unless explicitly
purged:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\uninstall-workstation.ps1
powershell -ExecutionPolicy Bypass -File scripts\uninstall-workstation.ps1 -PurgeData
```

## Safety and known Windows behavior

The agent never treats the generic Disk class as USB evidence and hard-allows keyboard, mouse,
HID, display, system, camera, Bluetooth, and network ancestry. Live CLI enforcement additionally
requires the exact instance ID. Default-deny uses Windows Device Installation Restrictions as a
service-down backstop; while the service is stopped, this can also deny other devices Windows marks
removable unless their observed installation IDs were approved. Existing external GPO/MDM values
are preserved and reported as conflicts.

Windows exposes a user-mode arrival race: a newly inserted device can be visible briefly before
active disable completes. The passive installation backstop reduces, but cannot eliminate, that
window. Validate production policy and hardware on every supported Windows build before rollout.

## Build and test

```powershell
dotnet restore UsbDlp.sln
dotnet build UsbDlp.sln --configuration Release --no-restore
dotnet test UsbDlp.sln --configuration Release --no-build
dotnet test tests\UsbDlp.Tests\UsbDlp.Tests.csproj --collect:"XPlat Code Coverage" `
  --settings tests\coverage.runsettings --results-directory TestResults
powershell -ExecutionPolicy Bypass -File eng\verify-policy-coverage.ps1 `
  -ResultsDirectory TestResults
```
