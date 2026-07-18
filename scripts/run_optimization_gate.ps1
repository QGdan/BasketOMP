param(
    [ValidateSet('O0', 'O1', 'O2', 'O3', 'O4', 'O5', 'O6')]
    [string]$Gate = 'O4',
    [switch]$RunMedium
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    & "$PSScriptRoot\run_correctness.ps1"
    if ($Gate -in @('O4', 'O5', 'O6')) {
        python "$PSScriptRoot\validate_results.py" --self-test
        if ($LASTEXITCODE -ne 0) { throw 'result validator self-test failed' }
    }
    if ($RunMedium -or $Gate -eq 'O6') {
        $raw = & "$PSScriptRoot\run_benchmark.ps1" -Dataset medium -Repeats 3 -Warmups 1 -ThreadCounts 1,2,4,8 -MaxNeighbors 50
        python "$PSScriptRoot\validate_results.py" $raw
        if ($LASTEXITCODE -ne 0) { throw 'result validation failed' }
        $summary = Join-Path (Split-Path $raw) 'summary.csv'
        python "$PSScriptRoot\summarize_results.py" $raw $summary
        if ($LASTEXITCODE -ne 0) { throw 'result summary failed' }
        python "$PSScriptRoot\plot_results.py" $summary (Join-Path (Split-Path $raw) 'figures')
        if ($LASTEXITCODE -ne 0) { throw 'result plotting failed' }
    }
    "PASS: optimization gate $Gate"
} finally {
    Pop-Location
}
