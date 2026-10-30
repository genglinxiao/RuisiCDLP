[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$kits = Join-Path $root "artifacts\legacy-kits"
$source = Join-Path $kits "RuisiUsbDlp-legacy-xp-x86-1.1.5-HF7"
$name = "RuisiUsbDlp-XP-SP3-x86-SetupAPI-1.1.6-TEST"
$destination = Join-Path $kits $name
$archive = Join-Path $kits "$name.zip"
$agent = Join-Path $root "installer\UsbDlp.Legacy.Agent.Installer\bin\x86\Release\RuisiUsbDlpXpSetupApiAgent-1.1.6-x86.msi"

foreach ($required in @($source, $agent)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing prerequisite: $required" }
}
if ((Test-Path -LiteralPath $destination) -or (Test-Path -LiteralPath $archive)) {
    throw "SetupAPI output already exists; refusing to overwrite it."
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
Move-Item -LiteralPath (Join-Path $destination "client\install-client.cmd") `
    -Destination (Join-Path $destination "client\install-agent-inner.cmd")
$install = @'
@echo off
setlocal EnableExtensions DisableDelayedExpansion
sc.exe query RuisiUsbDlpFilter >nul 2>&1
if not errorlevel 1 (
  echo The XP filter driver is installed. Restore a clean XP snapshot before testing SetupAPI.
  exit /b 3
)
sc.exe query UsbDlp.LegacyAgent >nul 2>&1
if not errorlevel 1 (
  echo Use a CLEAN XP SP3 snapshot with no prior Ruisi agent installed.
  exit /b 3
)
call "%~dp0install-agent-inner.cmd"
exit /b %errorlevel%
'@
Set-Content -LiteralPath (Join-Path $destination "client\install-client.cmd") -Value $install -Encoding Ascii

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
if not exist `"%AGENT%`" (
  echo Agent executable missing. Use Add/Remove Programs or restore the XP snapshot.
  exit /b 4
)
`"%AGENT%`" --restore-owned
if errorlevel 1 (
  echo An agent-owned disabled device could not be restored.
  echo Reconnect every tested USB stick and run this rollback script again.
  exit /b 6
)
msiexec.exe /x `"$productCode`" /qn /norestart
if errorlevel 1 (
  echo Agent MSI removal failed. Inspect Add/Remove Programs.
  exit /b 7
)
echo SetupAPI agent removed. Replug tested devices and verify access.
exit /b 0
"@
Set-Content -LiteralPath (Join-Path $destination "client\rollback-client.cmd") -Value $rollback -Encoding Ascii

$manifest = @(
    "Ruisi USB DLP Windows XP SP3 x86 SetupAPI experiment",
    "Target=windows-xp-sp3-x86",
    "Profile=legacy-xp-x86-setupapi",
    "QualificationStatus=UNVERIFIED_HARDWARE_TEST_ONLY",
    "Transport=HTTP_ISOLATED_TEST_ONLY",
    "AgentMode=LIVE_DEFAULT_DENY",
    "EnforcementBackend=SetupDiCallClassInstaller_DIF_PROPERTYCHANGE",
    "AgentVersion=1.1.6",
    "AgentProductCode=$productCode",
    "ServerVersion=1.1.2",
    "BuiltUtc=$([DateTime]::UtcNow.ToString('O'))"
)
Set-Content -LiteralPath (Join-Path $destination "MANIFEST.txt") -Value $manifest -Encoding Ascii

$guide = @(
    "XP SP3 x86 SetupAPI test track (experimental, NOT production DLP)",
    "Use a dedicated NTFS XP test machine or snapshot. Back up all test data.",
    "Do not install the SetupAPI and filter-driver tracks together.",
    "The server MSI is unchanged from HF7. Existing server installations need no upgrade.",
    "Edit config\deployment.ini ServerUrl and use the server's enrollment token.",
    "Run client\install-client.cmd as Administrator on a clean XP SP3 snapshot.",
    "The agent runs live default-deny, with DebugLog=1 in deployment.ini.",
    "Confirm legacy-agent.log has enforcement_backend=setupapi_propertychange.",
    "On first insertion, verify every drive letter is inaccessible before dismissing the notifier.",
    "Close the notifier, unplug/replug the SAME stick at least 20 times; test reads and writes.",
    "Repeat with a second stick and a multi-partition stick; check every volume.",
    "Confirm no USB hub, keyboard, mouse, or internal disk is disabled.",
    "Approve a request and verify access without service restart, at most after replug.",
    "Revoke approval and verify all drive letters are inaccessible after replug.",
    "Restart the agent and repeat deny/approve/revoke; ownership recovery must work.",
    "Any enforce_error or enforce_unverified means the deny was NOT confirmed.",
    "A blocked_verified log entry is not sufficient if file I/O still works.",
    "Send the full legacy-agent.log, XP Device Manager status, and a timestamped test record.",
    "If any bypass occurs, stop using this build as a security control and report it.",
    "Before uninstall, run client/rollback-client.cmd with all tested sticks connected."
)
Set-Content -LiteralPath (Join-Path $destination "docs\SETUPAPI-TEST-TRACK.txt") -Value $guide -Encoding Ascii
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
