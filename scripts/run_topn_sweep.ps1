param(
    [ValidateSet('small', 'medium')][string]$Dataset = 'medium',
    [int[]]$NeighborCounts = @(0, 20, 50, 100),
    [int]$Threads = 8,
    [int]$Repeats = 3,
    [int]$Warmups = 1
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    $sweepId = "$(Get-Date -Format 'yyyyMMdd-HHmmss')-$Dataset-topn-sweep"
    $sweepDir = Join-Path 'results\experiments' $sweepId
    New-Item -ItemType Directory -Force -Path $sweepDir | Out-Null
    $summaries = @()
    foreach ($neighbors in $NeighborCounts) {
        $raw = & "$PSScriptRoot\run_benchmark.ps1" -Dataset $Dataset -Repeats $Repeats -Warmups $Warmups -ThreadCounts $Threads -MaxNeighbors $neighbors
        python "$PSScriptRoot\validate_results.py" $raw
        if ($LASTEXITCODE -ne 0) { throw "validation failed for Top-$neighbors" }
        $summary = Join-Path (Split-Path $raw) 'summary.csv'
        python "$PSScriptRoot\summarize_results.py" $raw $summary
        if ($LASTEXITCODE -ne 0) { throw "summary failed for Top-$neighbors" }
        $summaries += $summary
    }
    python "$PSScriptRoot\compare_profiles.py" $sweepDir @summaries
    if ($LASTEXITCODE -ne 0) { throw 'profile comparison failed' }
    Write-Output $sweepDir
} finally {
    Pop-Location
}
