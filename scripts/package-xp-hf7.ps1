[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$kits = Join-Path $root "artifacts\legacy-kits"
$source = Join-Path $kits "RuisiUsbDlp-legacy-xp-x86-1.1.4-HF6"
$name = "RuisiUsbDlp-legacy-xp-x86-1.1.5-HF7"
$destination = Join-Path $kits $name
$archive = Join-Path $kits "RuisiUsbDlp-XP-SP3-x86-1.1.5-HF7-ENFORCEMENT-DEBUG-TEST-HANDOFF.zip"
$agentMsi = Join-Path $root "installer\UsbDlp.Legacy.Agent.Installer\bin\x86\Release\RuisiUsbDlpXpAgent-1.1.5-x86.msi"

foreach ($required in @($source, $agentMsi)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing prerequisite: $required" }
}
if ((Test-Path -LiteralPath $destination) -or (Test-Path -LiteralPath $archive)) {
    throw "HF7 output already exists; refusing to overwrite it."
}

Copy-Item -LiteralPath $source -Destination $destination -Recurse
$oldAgent = Join-Path $destination "client\RuisiUsbDlpXpAgent-1.1.4-x86.msi"
$oldHashes = Join-Path $destination "SHA256SUMS"
if (-not (Test-Path -LiteralPath $oldAgent -PathType Leaf)) {
    throw "Copied kit is missing the expected HF6 agent MSI."
}
Remove-Item -LiteralPath $oldAgent
Remove-Item -LiteralPath $oldHashes
Copy-Item -LiteralPath $agentMsi -Destination (Join-Path $destination "client")

$manifest = Get-Content -LiteralPath (Join-Path $source "MANIFEST.txt") |
    ForEach-Object {
        if ($_ -eq "Ruisi USB DLP Legacy Compatibility Kit 1.1.4 HF6") {
            "Ruisi USB DLP Legacy Compatibility Kit 1.1.5 HF7"
        } elseif ($_ -eq "AgentVersion=1.1.4") {
            "AgentVersion=1.1.5"
        } else { $_ }
    }
$manifest += "Hotfix7=HF7-correct-usbstor-disk-target-and-safety-ancestor"
$manifest += "Hotfix7Date=2026-10-18"
Set-Content -LiteralPath (Join-Path $destination "MANIFEST.txt") -Value $manifest -Encoding Ascii

$note = @(
    "HF7 Windows XP SP3 x86 test handoff",
    "Agent MSI: RuisiUsbDlpXpAgent-1.1.5-x86.msi",
    "Server MSI: unchanged from HF6 (1.1.2)",
    "Fix: only USBSTOR disk nodes are governed; USB parent/hub nodes are not disabled.",
    "Fix: a USB hub/controller in disk ancestry does not safety-allow the disk.",
    "This is a live default-deny test build. Use only an isolated XP test machine.",
    "Back up test data before installing; an attached USB card reader may also be denied.",
    "Install/upgrade using client\install-client.cmd as Administrator.",
    "Set config\deployment.ini ServerUrl; preserve the same enrollment token as the server.",
    "With DebugLog=1, insert a USB stick and collect legacy-agent.log.",
    "Expected: one USBSTOR disk device_seen decision=block reason=default_deny.",
    "Expected: debug_disable_attempt, then blocked or enforce_error with a result code.",
    "USB\\VID_* parent should be governed=false, and keyboard/hub must remain functional.",
    "Check all volumes on multi-partition media, then test approval and revocation.",
    "If the disk remains accessible, send the full log plus Device Manager status.",
    "Hardware behavior remains unverified until the external XP testers run this build."
)
Set-Content -LiteralPath (Join-Path $destination "docs\HOTFIX-HF7.txt") -Value $note -Encoding Ascii

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
