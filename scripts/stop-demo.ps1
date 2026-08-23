[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$statePath = Join-Path $root ".run\demo\processes.json"
if (-not (Test-Path -LiteralPath $statePath)) {
    Write-Host "No running demo state was found."
    exit 0
}
$state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
foreach ($id in @($state.notifier, $state.agent, $state.server)) {
    if ($id -and (Get-Process -Id $id -ErrorAction SilentlyContinue)) {
        Stop-Process -Id $id
    }
}
Remove-Item -LiteralPath $statePath
Write-Host "USB DLP demo processes stopped. Data and logs were preserved under .run\demo."
