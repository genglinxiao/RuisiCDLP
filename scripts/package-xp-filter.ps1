[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$kits = Join-Path $root "artifacts\legacy-kits"
$source = Join-Path $kits "RuisiUsbDlp-legacy-xp-x86-1.1.5-HF7"
$name = "RuisiUsbDlp-XP-SP3-x86-Filter-1.1.8-DIAGNOSTIC-TEST"
$destination = Join-Path $kits $name
$archive = Join-Path $kits "$name.zip"
$agent = Join-Path $root "installer\UsbDlp.Legacy.Agent.Installer\bin\x86\Release\RuisiUsbDlpXpFilterAgent-1.1.8-x86.msi"
$driver = Join-Path $root "legacy\build-xp-filter-driver\RuisiUsbDlpFilter.sys"
$helper = Join-Path $root "legacy\build-xp-filter\Release\UsbDlp.XpFilterSetup.exe"

foreach ($path in @($source, $agent, $driver, $helper, $destination, $archive)) {
    $full = [IO.Path]::GetFullPath($path)
    if (-not $full.StartsWith($root + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) { throw "Path outside workspace: $full" }
}
foreach ($required in @($source, $agent, $driver, $helper)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing prerequisite: $required" }
}
if ((Test-Path -LiteralPath $destination) -or (Test-Path -LiteralPath $archive)) {
    throw "Filter output already exists; refusing to overwrite it."
}

$installer = New-Object -ComObject WindowsInstaller.Installer
$database = $installer.OpenDatabase($agent, 0)
$view = $database.OpenView("SELECT ``Value`` FROM ``Property`` WHERE ``Property`` = 'ProductCode'")
$view.Execute()
$record = $view.Fetch()
if (-not $record) { throw "Agent MSI has no ProductCode." }
$productCode = $record.StringData(1)
$view.Close()

Copy-Item -LiteralPath $source -Destination $destination -Recurse
$oldAgent = Join-Path $destination "client\RuisiUsbDlpXpAgent-1.1.5-x86.msi"
if (-not (Test-Path -LiteralPath $oldAgent -PathType Leaf)) {
    throw "Copied kit is missing the expected HF7 agent MSI."
}
Remove-Item -LiteralPath $oldAgent
Remove-Item -LiteralPath (Join-Path $destination "SHA256SUMS")
Copy-Item -LiteralPath $agent -Destination (Join-Path $destination "client")
New-Item -ItemType Directory -Path (Join-Path $destination "driver") | Out-Null
Copy-Item -LiteralPath $driver -Destination (Join-Path $destination "driver")
Copy-Item -LiteralPath $helper -Destination (Join-Path $destination "driver")
Move-Item -LiteralPath (Join-Path $destination "client\install-client.cmd") `
    -Destination (Join-Path $destination "client\install-agent-inner.cmd")

$install = @'
@echo off
setlocal EnableExtensions DisableDelayedExpansion
net session >nul 2>&1
if errorlevel 1 (
  echo Run this script as Administrator.
  exit /b 5
)
sc.exe query UsbDlp.LegacyAgent >nul 2>&1
if not errorlevel 1 (
  echo Use a CLEAN XP SP3 snapshot with no prior Ruisi agent installed.
  echo Do not mix this track with SetupAPI or HF7 on the same XP image.
  exit /b 3
)
set "DRIVER=%~dp0..\driver\RuisiUsbDlpFilter.sys"
set "HELPER=%~dp0..\driver\UsbDlp.XpFilterSetup.exe"
"%HELPER%" install "%DRIVER%"
if errorlevel 1 (
  echo Filter registration failed. No agent was installed.
  exit /b 4
)
call "%~dp0install-agent-inner.cmd"
if errorlevel 1 (
  echo Agent installation failed. Removing filter registration.
  msiexec.exe /x "__AGENT_PRODUCT_CODE__" /qn /norestart
  "%HELPER%" uninstall
  echo RESTART XP before using the machine again.
  exit /b 6
)
echo Filter and agent registered. RESTART XP before testing any USB device.
echo This build is for isolated XP lab tests only.
exit /b 0
'@
$install = $install.Replace("__AGENT_PRODUCT_CODE__", $productCode)
Set-Content -LiteralPath (Join-Path $destination "client\install-client.cmd") -Value $install -Encoding Ascii

$diagnose = @'
@echo off
setlocal EnableExtensions DisableDelayedExpansion
"%~dp0..\driver\UsbDlp.XpFilterSetup.exe" diagnose
if errorlevel 1 (
  echo FILTER HEALTH CHECK FAILED. Do not treat the USB device as protected.
  echo Send this output and the XP System Event Log to the development team.
  exit /b 1
)
echo Driver is reachable. Repeat this check after inserting the USB disk.
echo attached_usb_disks must be nonzero and agent log must report stage=attached.
echo Then test every volume, including after manual enable.
exit /b 0
'@
Set-Content -LiteralPath (Join-Path $destination "client\diagnose-filter.cmd") -Value $diagnose -Encoding Ascii

$rollback = @"
@echo off
setlocal EnableExtensions DisableDelayedExpansion
net session >nul 2>&1
if errorlevel 1 (
  echo Run this script as Administrator.
  exit /b 5
)
sc.exe stop UsbDlp.LegacyAgent >nul 2>&1
set `"AGENT=%ProgramFiles%\Ruisi USB DLP Legacy\UsbDlp.LegacyAgent.exe`"
if exist `"%AGENT%`" (
  `"%AGENT%`" --restore-owned
  if errorlevel 1 (
    echo Reconnect every agent-owned WPD device and retry rollback.
    exit /b 6
  )
)
msiexec.exe /x `"$productCode`" /qn /norestart
if errorlevel 1 (
  echo Agent MSI removal did not complete; inspect Add/Remove Programs.
  exit /b 7
)
`"%~dp0..\driver\UsbDlp.XpFilterSetup.exe`" uninstall
if errorlevel 1 (
  echo Filter removal failed. Restore the XP test snapshot.
  exit /b 6
)
echo RESTART XP to detach the filter. Verify the disk-class UpperFilters value.
exit /b 0
"@
Set-Content -LiteralPath (Join-Path $destination "client\rollback-client.cmd") -Value $rollback -Encoding Ascii

$manifest = @(
    "Ruisi USB DLP Windows XP SP3 x86 kernel filter experiment",
    "Target=windows-xp-sp3-x86",
    "Profile=legacy-xp-x86-filter",
    "QualificationStatus=UNVERIFIED_KERNEL_HARDWARE_TEST_ONLY",
    "Transport=HTTP_ISOLATED_TEST_ONLY",
    "AgentMode=LIVE_DEFAULT_DENY",
    "EnforcementBackend=USBSTOR_disk_class_upper_filter",
    "AgentVersion=1.1.8",
    "ServerVersion=1.1.2",
    "Driver=RuisiUsbDlpFilter.sys",
    "DriverInstall=client/install-client.cmd",
    "DriverRollback=client/rollback-client.cmd",
    "AgentProductCode=$productCode",
    "BuiltUtc=$([DateTime]::UtcNow.ToString('O'))"
)
Set-Content -LiteralPath (Join-Path $destination "MANIFEST.txt") -Value $manifest -Encoding Ascii

$guide = @(
    "READ FIRST: Windows XP SP3 x86 KERNEL FILTER LAB TRACK. NOT PRODUCTION DLP.",
    "Only use a dedicated XP SP3 x86 NTFS VM or sacrificial lab PC with a full snapshot/image.",
    "A disk-class filter can make XP unbootable if installation or removal fails.",
    "Keep XP recovery media and a backup of test data. Do not use on a production machine.",
    "Use a CLEAN XP snapshot; do not mix this track with HF7 or SetupAPI builds.",
    "Edit config/deployment.ini ServerUrl and replace the test enrollment token and admin password.",
    "Run client/install-client.cmd as Administrator, then REBOOT before inserting a USB disk.",
    "After reboot, run client/diagnose-filter.cmd and stop if filter_health is not READY_FOR_BYPASS_TEST.",
    "After USB insertion, run diagnose-filter.cmd again: attached_usb_disks must be nonzero.",
    "The driver attaches only when the disk PDO enumerator is USBSTOR, default denied.",
    "The agent grants only the exact device when server policy allows it; server loss leaves cached policy in effect.",
    "Do not infer success from a popup. Verify no file reads/writes work on every volume.",
    "Watch legacy-agent.log for enforcement_backend=xp_disk_filter, blocked_verified or enforce_unverified.",
    "Test 20 unplug/replug cycles, two different sticks, one multi-LUN stick, service restart, reboot.",
    "Approve and revoke from the server; replug may be required after each policy change.",
    "Test internal disk, keyboard, mouse, USB hub and non-storage devices still work.",
    "Send complete agent log, XP Event Viewer System log, Device Manager state and timestamped test record.",
    "To rollback: run client/rollback-client.cmd as Administrator, REBOOT, confirm disk access.",
    "If XP does not boot, restore the snapshot or use recovery media to remove the UpperFilters entry.",
    "Any bypass, crash, boot issue or unverified state fails this track. Stop testing and report it.",
    "The prior 1.1.7 build did not produce any verified block in the supplied tester log."
)
Set-Content -LiteralPath (Join-Path $destination "docs\READ-FIRST-FILTER.txt") -Value $guide -Encoding Ascii
$chineseHandoff = Join-Path $root "docs\XP-SP3-two-track-tester-handoff-zh-CN.md"
$chineseManual = Join-Path $root "docs\Ruisi-USB-DLP-XP-Tester-Manual-zh-CN.pdf"
if (-not (Test-Path -LiteralPath $chineseHandoff) -or
    -not (Test-Path -LiteralPath $chineseManual)) { throw "Missing Chinese tester documentation." }
Get-Content -LiteralPath $chineseHandoff -Raw -Encoding UTF8 |
    Set-Content -LiteralPath (Join-Path $destination "docs\READ-FIRST-ZH-CN.txt") -Encoding Unicode
Copy-Item -LiteralPath $chineseManual -Destination (Join-Path $destination "docs")

$hashLines = Get-ChildItem -LiteralPath $destination -File -Recurse |
    Sort-Object FullName |
    ForEach-Object {
        $hash = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
        $relative = $_.FullName.Substring($destination.Length + 1).Replace("\", "/")
        "$($hash.Hash.ToLowerInvariant())  $relative"
    }
Set-Content -LiteralPath (Join-Path $destination "SHA256SUMS") -Value $hashLines -Encoding Ascii
Compress-Archive -LiteralPath $destination -DestinationPath $archive
$archiveHash = Get-FileHash -LiteralPath $archive -Algorithm SHA256
Set-Content -LiteralPath "$archive.sha256" -Value "$($archiveHash.Hash.ToLowerInvariant())  $(Split-Path $archive -Leaf)" -Encoding Ascii
Write-Host "Kit: $archive"
Write-Host "SHA256: $($archiveHash.Hash.ToLowerInvariant())"
