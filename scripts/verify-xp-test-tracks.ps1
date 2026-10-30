[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$kits = Join-Path $root "artifacts\legacy-kits"
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Assert-XpPe([string] $Path, [int] $ExpectedSubsystem) {
    $data = [IO.File]::ReadAllBytes($Path)
    if ($data.Length -lt 512 -or [BitConverter]::ToUInt16($data, 0) -ne 0x5a4d) {
        throw "Not an MZ executable: $Path"
    }
    $pe = [BitConverter]::ToInt32($data, 0x3c)
    if ($pe -lt 0 -or $pe + 256 -gt $data.Length -or
        [BitConverter]::ToUInt32($data, $pe) -ne 0x00004550 -or
        [BitConverter]::ToUInt16($data, $pe + 4) -ne 0x014c -or
        [BitConverter]::ToUInt16($data, $pe + 24) -ne 0x010b) {
        throw "Not a 32-bit x86 PE: $Path"
    }
    $optional = $pe + 24
    $major = [BitConverter]::ToUInt16($data, $optional + 48)
    $minor = [BitConverter]::ToUInt16($data, $optional + 50)
    $subsystem = [BitConverter]::ToUInt16($data, $optional + 68)
    $clrRva = [BitConverter]::ToUInt32($data, $optional + 208)
    if ($major -ne 5 -or $minor -ne 1 -or $subsystem -ne $ExpectedSubsystem -or $clrRva -ne 0) {
        throw "Wrong XP subsystem or CLR payload: $Path ($major.$minor, subsystem $subsystem, CLR $clrRva)"
    }
}

$installer = New-Object -ComObject WindowsInstaller.Installer
foreach ($track in @(
    @{ Name = "RuisiUsbDlp-XP-SP3-x86-SetupAPI-1.1.6-TEST"; Version = "1.1.6"; Msi = "RuisiUsbDlpXpSetupApiAgent-1.1.6-x86.msi" },
    @{ Name = "RuisiUsbDlp-XP-SP3-x86-Filter-1.1.8-DIAGNOSTIC-TEST"; Version = "1.1.8"; Msi = "RuisiUsbDlpXpFilterAgent-1.1.8-x86.msi" }
)) {
    $folder = Join-Path $kits $track.Name
    $archive = "$folder.zip"
    $hashFile = "$archive.sha256"
    foreach ($file in @($folder, $archive, $hashFile)) {
        if (-not (Test-Path -LiteralPath $file)) { throw "Missing $file" }
    }
    $expectedArchive = (Get-Content -LiteralPath $hashFile -Raw).Trim().Split(" ")[0]
    $actualArchive = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualArchive -ne $expectedArchive) { throw "ZIP SHA-256 mismatch: $archive" }
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        $entries = @{}
        foreach ($entry in $zip.Entries) {
            if ($entry.Name) { $entries[$entry.FullName.Replace("\", "/")] = $entry }
        }
        $manifest = Get-Content -LiteralPath (Join-Path $folder "SHA256SUMS")
        foreach ($line in $manifest) {
            if ($line -notmatch '^([0-9a-f]{64})  (.+)$') { throw "Bad SHA256SUMS line: $line" }
            $expected = $Matches[1]
            $relative = $Matches[2]
            $file = Join-Path $folder ($relative.Replace("/", "\"))
            if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing kit file $file" }
            $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actual -ne $expected) { throw "Kit file hash mismatch: $file" }
            $key = "$($track.Name)/$relative"
            if (-not $entries.ContainsKey($key)) { throw "Missing ZIP entry: $key" }
            $stream = $entries[$key].Open()
            try {
                $sha = [Security.Cryptography.SHA256]::Create()
                try { $zipHash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace("-", "").ToLowerInvariant() }
                finally { $sha.Dispose() }
            } finally { $stream.Dispose() }
            if ($zipHash -ne $expected) { throw "ZIP content mismatch: $key" }
        }
        if ($entries.Count -ne $manifest.Count + 1) {
            throw "Unexpected ZIP file count for $($track.Name): $($entries.Count)"
        }
    } finally { $zip.Dispose() }

    $msi = Join-Path $folder "client\$($track.Msi)"
    $database = $installer.OpenDatabase($msi, 0)
    $view = $database.OpenView('SELECT `Property`, `Value` FROM `Property`')
    $view.Execute()
    $properties = @{}
    while ($record = $view.Fetch()) { $properties[$record.StringData(1)] = $record.StringData(2) }
    $view.Close()
    if ($properties.ProductVersion -ne $track.Version -or
        $properties.UpgradeCode -ne '{25B625A0-F0E2-4F74-B1C4-833ABDFB022A}' -or
        -not $properties.ProductCode) { throw "Wrong agent MSI identity: $msi" }
    $filesView = $database.OpenView('SELECT `FileName` FROM `File`')
    $filesView.Execute()
    $payloadNames = @()
    while ($record = $filesView.Fetch()) {
        $payloadNames += ($record.StringData(1) -split '\|')[-1]
    }
    $filesView.Close()
    foreach ($requiredPayload in @('UsbDlp.LegacyAgent.exe', 'UsbDlp.LegacyNotifier.exe')) {
        if ($requiredPayload -notin $payloadNames) { throw "Missing MSI payload: $requiredPayload" }
    }
    $serverMsi = Join-Path $folder 'server\RuisiUsbDlpXpServer-1.1.2-x86.msi'
    $serverDatabase = $installer.OpenDatabase($serverMsi, 0)
    $serverView = $serverDatabase.OpenView("SELECT ``Value`` FROM ``Property`` WHERE ``Property`` = 'ProductVersion'")
    $serverView.Execute()
    $serverRecord = $serverView.Fetch()
    if (-not $serverRecord -or $serverRecord.StringData(1) -ne '1.1.2') {
        throw "Wrong server MSI version: $serverMsi"
    }
    $serverView.Close()
    foreach ($relative in @("server\install-server.cmd", "server\RuisiUsbDlpXpServer-1.1.2-x86.msi",
                            "client\install-client.cmd", "client\rollback-client.cmd",
                            "docs\READ-FIRST-ZH-CN.txt", "docs\Ruisi-USB-DLP-XP-Tester-Manual-zh-CN.pdf")) {
        if (-not (Test-Path -LiteralPath (Join-Path $folder $relative) -PathType Leaf)) {
            throw "Missing tester deliverable: $relative"
        }
    }
    if ($track.Version -eq '1.1.8' -and
        -not (Test-Path -LiteralPath (Join-Path $folder 'client\diagnose-filter.cmd') -PathType Leaf)) {
        throw 'Missing XP filter health-check command.'
    }
    Write-Host "$($track.Name): $($manifest.Count) files verified, ZIP SHA256 $actualArchive, MSI $($properties.ProductVersion)"
}

foreach ($pe in @(
    @{ Path = "legacy\build-xp-setupapi\Release\UsbDlp.LegacyAgent.exe"; Subsystem = 3 },
    @{ Path = "legacy\build-xp-filter\Release\UsbDlp.LegacyAgent.exe"; Subsystem = 3 },
    @{ Path = "legacy\build-xp-filter\Release\UsbDlp.XpFilterSetup.exe"; Subsystem = 3 },
    @{ Path = "legacy\build-xp-filter-driver\RuisiUsbDlpFilter.sys"; Subsystem = 1 }
)) {
    Assert-XpPe (Join-Path $root $pe.Path) $pe.Subsystem
}
Write-Host "All standalone XP executables and the driver are x86, subsystem 5.01, and non-CLR."
