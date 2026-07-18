param(
    [ValidateSet('O0', 'O1', 'O2', 'O3', 'O4', 'O5', 'O6')]
    [string]$Gate = 'O4',
    [switch]$RunMedium,
    [int]$MaxThreads = 0,
    [ValidateSet('spread', 'close', 'master')]
    [string]$ProcBind = 'spread',
    [ValidateSet('cores', 'threads', 'sockets')]
    [string]$Places = 'cores'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$python = $null
$candidates = @('python', 'python3', 'py')
foreach ($cmd in $candidates) {
    $found = Get-Command $cmd -ErrorAction SilentlyContinue
    if ($found) { $python = $found.Source; break }
}

Push-Location $projectRoot
try {
    & "$PSScriptRoot\run_correctness.ps1"

    if ($Gate -in @('O4', 'O5', 'O6') -and $python) {
        & $python "$PSScriptRoot\validate_results.py" --self-test
        if ($LASTEXITCODE -ne 0) { throw '验证器自测失败' }
    }

    if ($RunMedium -or $Gate -eq 'O6') {
        $detectedThreads = [Environment]::ProcessorCount
        $maxTh = if ($MaxThreads -gt 0) { $MaxThreads } else { $detectedThreads }
        $threadList = @(1)
        $v = 2
        while ($v -lt $maxTh) { $threadList += $v; $v *= 2 }
        if ($threadList[-1] -ne $maxTh) { $threadList += $maxTh }

        $raw = & "$PSScriptRoot\run_benchmark.ps1" `
            -Dataset medium -Repeats 3 -Warmups 1 `
            -MaxNeighbors 50 -MaxThreads $maxTh `
            -ThreadCounts $threadList `
            -ProcBind $ProcBind -Places $Places `
            -NoPostProcess

        if ($python) {
            & $python "$PSScriptRoot\validate_results.py" $raw
            if ($LASTEXITCODE -ne 0) { throw '结果验证失败' }

            $summary = Join-Path (Split-Path $raw) 'summary.csv'
            & $python "$PSScriptRoot\summarize_results.py" $raw $summary
            if ($LASTEXITCODE -ne 0) { throw '汇总生成失败' }

            & $python "$PSScriptRoot\plot_results.py" $summary (Join-Path (Split-Path $raw) 'figures')
            if ($LASTEXITCODE -ne 0) { throw '图表生成失败' }
        }
    }
    "PASS: optimization gate $Gate"
} finally {
    Pop-Location
}
