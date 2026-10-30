[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$outputRoot = Join-Path $root "artifacts\legacy-kits"
$agentProject = Join-Path $root "installer\UsbDlp.Legacy.Agent.Installer\UsbDlp.Legacy.Agent.Installer.wixproj"
$serverProject = Join-Path $root "installer\UsbDlp.Legacy.Server.Installer\UsbDlp.Legacy.Server.Installer.wixproj"
$templates = Join-Path $root "legacy\packaging"
$env:DOTNET_CLI_HOME = Join-Path $root ".dotnet"
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = "1"
$env:DOTNET_NOLOGO = "1"

foreach ($targetProfile in @("win7", "win10")) {
    foreach ($platform in @("x86", "x64")) {
        dotnet build $agentProject -c Release -p:Platform=$platform -p:TargetProfile=$targetProfile --no-restore
        if ($LASTEXITCODE -ne 0) { throw "The $targetProfile $platform agent MSI build failed." }
        dotnet build $serverProject -c Release -p:Platform=$platform -p:TargetProfile=$targetProfile --no-restore
        if ($LASTEXITCODE -ne 0) { throw "The $targetProfile $platform server MSI build failed." }
    }
}

dotnet build $agentProject -c Release -p:Platform=x86 -p:TargetProfile=xp --no-restore
if ($LASTEXITCODE -ne 0) { throw "The Windows XP x86 agent MSI build failed." }
dotnet build $serverProject -c Release -p:Platform=x86 -p:TargetProfile=xp --no-restore
if ($LASTEXITCODE -ne 0) { throw "The Windows XP x86 server MSI build failed." }

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

function New-CompatibilityKit {
    param(
        [Parameter(Mandatory = $true)][string] $OsName,
        [Parameter(Mandatory = $true)][ValidateSet("x86", "x64")][string] $Architecture,
        [Parameter(Mandatory = $true)][ValidateSet("win7", "win10", "xp")][string] $TargetProfile
    )

    $profile = if ($TargetProfile -eq "xp") { "legacy-xp-x86" } else { "legacy-$TargetProfile-$Architecture" }
    $kitName = "RuisiUsbDlp-$profile-1.1.0"
    $directory = Join-Path $outputRoot $kitName
    if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $configDirectory = New-Item -ItemType Directory -Path (Join-Path $directory "config") -Force
    $clientDirectory = New-Item -ItemType Directory -Path (Join-Path $directory "client") -Force
    $serverDirectory = New-Item -ItemType Directory -Path (Join-Path $directory "server") -Force
    $docsDirectory = New-Item -ItemType Directory -Path (Join-Path $directory "docs") -Force

    if ($TargetProfile -eq "xp") {
        $agentMsi = Join-Path $root "installer\UsbDlp.Legacy.Agent.Installer\bin\x86\Release\RuisiUsbDlpXpAgent-1.1.5-x86.msi"
        $serverMsi = Join-Path $root "installer\UsbDlp.Legacy.Server.Installer\bin\x86\Release\RuisiUsbDlpXpServer-1.1.2-x86.msi"
    } elseif ($TargetProfile -eq "win10") {
        $agentMsi = Join-Path $root "installer\UsbDlp.Legacy.Agent.Installer\bin\$Architecture\Release\RuisiUsbDlpWin10Agent-1.1.0-$Architecture.msi"
        $serverMsi = Join-Path $root "installer\UsbDlp.Legacy.Server.Installer\bin\$Architecture\Release\RuisiUsbDlpWin10Server-1.1.0-$Architecture.msi"
    } else {
        $agentMsi = Join-Path $root "installer\UsbDlp.Legacy.Agent.Installer\bin\$Architecture\Release\RuisiUsbDlpLegacyAgent-1.1.0-$Architecture.msi"
        $serverMsi = Join-Path $root "installer\UsbDlp.Legacy.Server.Installer\bin\$Architecture\Release\RuisiUsbDlpLegacyServer-1.1.0-$Architecture.msi"
    }
    foreach ($package in @($agentMsi, $serverMsi)) {
        if (-not (Test-Path -LiteralPath $package -PathType Leaf)) { throw "Missing package: $package" }
    }

    # Windows Installer records the original package name and expects it during same-version repair.
    Copy-Item -LiteralPath $agentMsi -Destination (Join-Path $clientDirectory (Split-Path $agentMsi -Leaf)) -Force
    Copy-Item -LiteralPath $serverMsi -Destination (Join-Path $serverDirectory (Split-Path $serverMsi -Leaf)) -Force
    Copy-Item -LiteralPath (Join-Path $templates "Install-Client.cmd") -Destination (Join-Path $clientDirectory "install-client.cmd") -Force
    Copy-Item -LiteralPath (Join-Path $templates "Install-Server.cmd") -Destination (Join-Path $serverDirectory "install-server.cmd") -Force
    Copy-Item -LiteralPath (Join-Path $templates "Admin-Server.cmd") -Destination (Join-Path $serverDirectory "admin-server.cmd") -Force
    Copy-Item -LiteralPath (Join-Path $templates "EnrollmentTokenFile.txt") -Destination (Join-Path $configDirectory "enrollment-token.txt") -Force
    Copy-Item -LiteralPath (Join-Path $templates "AdminPassword.txt") -Destination (Join-Path $configDirectory "admin-password.txt") -Force
    Copy-Item -LiteralPath (Join-Path $templates "AdministratorGuide.txt") -Destination (Join-Path $docsDirectory "AdministratorGuide.txt") -Force
    if ($TargetProfile -eq "xp") {
        foreach ($extension in @("docx", "pdf")) {
            $manual = Join-Path $root "docs\Ruisi-USB-DLP-XP-Tester-Manual.$extension"
            if (-not (Test-Path -LiteralPath $manual -PathType Leaf)) {
                throw "Missing XP tester manual: $manual"
            }
            Copy-Item -LiteralPath $manual -Destination $docsDirectory -Force
        }
    }
    @(
        "[Deployment]",
        "ServerUrl=http://192.168.1.10:5080",
        "Port=5080",
        "AdminUsername=admin",
        "DebugLog=1",
        "Profile=$profile"
    ) | Set-Content -LiteralPath (Join-Path $configDirectory "deployment.ini") -Encoding Ascii

    $manifest = @(
        "Ruisi USB DLP Legacy Compatibility Kit 1.1.0",
        "Target=$OsName $Architecture",
        "Profile=$profile",
        "QualificationStatus=UNVERIFIED",
        "Transport=HTTP_ISOLATED_TEST_ONLY",
        "AgentMode=LIVE_DEFAULT_DENY",
        "BuiltUtc=$([DateTime]::UtcNow.ToString('O'))"
    )
    Set-Content -LiteralPath (Join-Path $directory "MANIFEST.txt") -Value $manifest -Encoding Ascii

    $hashLines = Get-ChildItem -LiteralPath $directory -File -Recurse |
        Where-Object Name -ne "SHA256SUMS" |
        Sort-Object FullName |
        ForEach-Object {
            $hash = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
            $relative = $_.FullName.Substring($directory.Length + 1).Replace("\", "/")
            "$($hash.Hash.ToLowerInvariant())  $relative"
        }
    Set-Content -LiteralPath (Join-Path $directory "SHA256SUMS") -Value $hashLines -Encoding Ascii

    $archive = Join-Path $outputRoot "$kitName.zip"
    Compress-Archive -Path $directory -DestinationPath $archive -Force
    Write-Host "Kit: $archive"
}

New-CompatibilityKit -OsName "windows-7" -Architecture "x86" -TargetProfile "win7"
New-CompatibilityKit -OsName "windows-7" -Architecture "x64" -TargetProfile "win7"
New-CompatibilityKit -OsName "windows-10" -Architecture "x86" -TargetProfile "win10"
New-CompatibilityKit -OsName "windows-10" -Architecture "x64" -TargetProfile "win10"
New-CompatibilityKit -OsName "windows-xp" -Architecture "x86" -TargetProfile "xp"
