[CmdletBinding()]
param(
    [string] $ResultsDirectory = "TestResults",
    [double] $MinimumBranchRate = 0.95
)

$reports = @(Get-ChildItem -LiteralPath $ResultsDirectory -Filter "coverage.cobertura.xml" -Recurse)
if ($reports.Count -ne 1) {
    throw "Expected one Cobertura report under '$ResultsDirectory'; found $($reports.Count)."
}

[xml] $coverage = Get-Content -LiteralPath $reports[0].FullName
$policyEvaluator = @(
    $coverage.coverage.packages.package.classes.class |
        Where-Object { $_.name -eq "UsbDlp.Policy.PolicyEvaluator" }
)

if ($policyEvaluator.Count -ne 1) {
    throw "Expected one coverage entry for UsbDlp.Policy.PolicyEvaluator; found $($policyEvaluator.Count)."
}

$branchRate = [double]::Parse(
    $policyEvaluator[0].'branch-rate',
    [Globalization.CultureInfo]::InvariantCulture)
$percentage = $branchRate.ToString("P2", [Globalization.CultureInfo]::InvariantCulture)
Write-Host "PolicyEvaluator branch coverage: $percentage"

if ($branchRate -lt $MinimumBranchRate) {
    throw "PolicyEvaluator branch coverage $percentage is below the required $($MinimumBranchRate.ToString('P0'))."
}
