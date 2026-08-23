[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $WorkspaceRoot,
    [Parameter(Mandatory = $true)]
    [string] $PidPath
)

$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath($WorkspaceRoot)
$runDirectory = Join-Path $root ".run\demo"
$agentExecutable = Join-Path $root "src\UsbDlp.Agent\bin\Release\net8.0-windows\UsbDlp.Agent.exe"
$notifierExecutable = Join-Path $root "src\UsbDlp.Notifier\bin\Release\net8.0-windows\UsbDlp.Notifier.exe"

$env:UsbDlp__ServerBaseUrl = "http://127.0.0.1:5091"
$env:UsbDlp__DataDirectory = Join-Path $runDirectory "agent"
$env:UsbDlp__DryRun = "false"
$env:UsbDlp__AllowInsecureHttp = "true"
$env:UsbDlp__LocalPollSeconds = "2"
$env:UsbDlp__SimulatedDevicePath = ""
$env:UsbDlp__NotifierExecutablePath = $notifierExecutable

$agent = Start-Process `
    -FilePath $agentExecutable `
    -WorkingDirectory $root `
    -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $runDirectory "agent-live.log") `
    -RedirectStandardError (Join-Path $runDirectory "agent-live.err.log") `
    -PassThru
$agent.Id | Set-Content -LiteralPath $PidPath -Encoding ASCII
