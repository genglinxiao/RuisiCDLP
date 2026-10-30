[CmdletBinding()]
param(
    [string] $ImageDirectory = "artifacts\toolchain\wdk7-image\WDK",
    [string] $OutputDirectory = "artifacts\toolchain\wdk7-extracted-correct",
    [string[]] $Components = @("headers", "libs_x86fre", "buildtools_x86fre")
)

$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$image = [IO.Path]::GetFullPath((Join-Path $root $ImageDirectory))
$output = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
if (-not $image.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
    -not $output.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Input and output must remain inside the workspace."
}
if (Test-Path -LiteralPath $output) { throw "Output already exists: $output" }
New-Item -ItemType Directory -Path $output | Out-Null

$installer = New-Object -ComObject WindowsInstaller.Installer
foreach ($component in $Components) {
    $msi = Join-Path $image "$component.msi"
    $cab = Join-Path $image "${component}_cab001.cab"
    if (-not (Test-Path -LiteralPath $msi -PathType Leaf) -or
        -not (Test-Path -LiteralPath $cab -PathType Leaf)) {
        throw "Missing WDK component: $component"
    }
    $stage = Join-Path $output "_stage_$component"
    New-Item -ItemType Directory -Path $stage | Out-Null
    & expand.exe '-F:*' $cab $stage | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "CAB extraction failed: $component" }

    $db = $installer.OpenDatabase($msi, 0)
    $directories = @{}
    $view = $db.OpenView('SELECT `Directory`, `Directory_Parent`, `DefaultDir` FROM `Directory`')
    $view.Execute()
    while ($record = $view.Fetch()) {
        $directories[$record.StringData(1)] = @($record.StringData(2), $record.StringData(3))
    }
    $view.Close()
    $componentDirectories = @{}
    $view = $db.OpenView('SELECT `Component`, `Directory_` FROM `Component`')
    $view.Execute()
    while ($record = $view.Fetch()) {
        $componentDirectories[$record.StringData(1)] = $record.StringData(2)
    }
    $view.Close()
    $view = $db.OpenView('SELECT `File`, `FileName`, `Component_` FROM `File`')
    $view.Execute()
    $count = 0
    while ($record = $view.Fetch()) {
        $key = $record.StringData(1)
        $name = ($record.StringData(2) -split '\|')[-1]
        $directory = $componentDirectories[$record.StringData(3)]
        if (-not $directory) { throw "Missing component directory for $key" }
        $segments = @()
        while ($directory -and $directory -ne 'DEFAULT_INSTALL_DIR1') {
            if (-not $directories.ContainsKey($directory)) { throw "Missing directory $directory" }
            $entry = $directories[$directory]
            $segment = ($entry[1] -split '\|')[-1]
            if ($segment -and $segment -ne '.') { $segments = @($segment) + $segments }
            $directory = $entry[0]
        }
        if ($directory -ne 'DEFAULT_INSTALL_DIR1') { throw "Unexpected WDK root for $key" }
        $relative = if ($segments.Count -eq 0) { $name } else {
            Join-Path -Path ($segments -join '\') -ChildPath $name
        }
        $target = [IO.Path]::GetFullPath((Join-Path $output $relative))
        if (-not $target.StartsWith($output + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe path: $relative" }
        $source = [IO.Path]::GetFullPath((Join-Path $stage $key))
        if (-not $source.StartsWith($stage + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe CAB key: $key" }
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing CAB payload: $key" }
        $parent = Split-Path -Parent $target
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
        Move-Item -LiteralPath $source -Destination $target
        $count++
    }
    $view.Close()
    Remove-Item -LiteralPath $stage
    Write-Host "Extracted $count WDK files from $component"
}
Write-Host "WDK component tree: $output"
