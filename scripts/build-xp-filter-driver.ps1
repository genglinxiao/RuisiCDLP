[CmdletBinding()]
param(
    [string] $WdkDirectory = "artifacts\toolchain\wdk7-extracted-correct"
)

$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$wdk = [IO.Path]::GetFullPath((Join-Path $root $WdkDirectory))
if (-not $wdk.StartsWith($root + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "WDK tree must be inside the workspace."
}
$output = Join-Path $root "legacy\build-xp-filter-driver"
New-Item -ItemType Directory -Path $output -Force | Out-Null
$compiler = Join-Path $wdk "bin\x86\x86\cl.exe"
$linker = Join-Path $wdk "bin\x86\x86\link.exe"
$source = Join-Path $root "legacy\driver\xp_filter.c"
$object = Join-Path $output "RuisiUsbDlpFilter.obj"
$driver = Join-Path $output "RuisiUsbDlpFilter.sys"
foreach ($file in @($compiler, $linker, $source)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing $file" }
}
$env:PATH = "$(Join-Path $wdk 'bin\x86\x86');$env:PATH"
& $compiler /nologo /c /W4 /WX /Zl /Oi /Gy /Gz /D_X86_=1 /Di386=1 `
    /D_XP_=1 /DNTDDI_VERSION=0x05010300 /DWINVER=0x0501 `
    /D_WIN32_WINNT=0x0501 "/I$(Join-Path $wdk 'inc\ddk')" `
    "/I$(Join-Path $wdk 'inc\api')" "/I$(Join-Path $wdk 'inc\crt')" `
    "/I$(Join-Path $root 'legacy\include')" "/Fo$object" $source
if ($LASTEXITCODE -ne 0) { throw "XP driver compilation failed." }
& $linker /nologo /driver /subsystem:native,5.01 /entry:DriverEntry `
    /nodefaultlib /machine:x86 "/out:$driver" $object `
    (Join-Path $wdk 'lib\win7\i386\ntoskrnl.lib') `
    (Join-Path $wdk 'lib\win7\i386\hal.lib') `
    (Join-Path $wdk 'lib\wxp\i386\wdmsec.lib') `
    (Join-Path $wdk 'lib\wxp\i386\BufferOverflowK.lib')
if ($LASTEXITCODE -ne 0) { throw "XP driver link failed." }
$hash = Get-FileHash -LiteralPath $driver -Algorithm SHA256
Write-Host "Driver: $driver"
Write-Host "SHA256: $($hash.Hash.ToLowerInvariant())"
