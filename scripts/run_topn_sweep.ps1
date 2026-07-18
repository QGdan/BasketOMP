param(
    [ValidateSet('small', 'medium')][string]$Dataset = 'medium',
    [int[]]$NeighborCounts = @(0, 20, 50, 100),
    [int]$Threads = 8,
    [int]$Repeats = 3,
    [int]$Warmups = 1,
    [ValidateSet('spread', 'close', 'master')]
    [string]$ProcBind = 'spread',
    [ValidateSet('cores', 'threads', 'sockets')]
    [string]$Places = 'cores'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$python = $null
foreach ($cmd in @('python', 'python3', 'py')) {
    $found = Get-Command $cmd -ErrorAction SilentlyContinue
    if ($found) { $python = $found.Source; break }
}
if (-not $python) { throw '未找到 Python，无法运行 sweep 后处理' }

Push-Location $projectRoot
try {
    $sweepId = "$(Get-Date -Format 'yyyyMMdd-HHmmss')-$Dataset-topn-sweep"
    $sweepDir = Join-Path 'results\experiments' $sweepId
    New-Item -ItemType Directory -Force -Path $sweepDir | Out-Null
    $summaries = @()
    foreach ($neighbors in $NeighborCounts) {
        Write-Output "── Top-$neighbors ──"
        $raw = & "$PSScriptRoot\run_benchmark.ps1" `
            -Dataset $Dataset -Repeats $Repeats -Warmups $Warmups `
            -ThreadCounts $Threads -MaxNeighbors $neighbors `
            -ProcBind $ProcBind -Places $Places `
            -NoPostProcess
        & $python "$PSScriptRoot\validate_results.py" $raw
        if ($LASTEXITCODE -ne 0) { throw "Top-$neighbors 验证失败" }
        $summary = Join-Path (Split-Path $raw) 'summary.csv'
        & $python "$PSScriptRoot\summarize_results.py" $raw $summary
        if ($LASTEXITCODE -ne 0) { throw "Top-$neighbors 汇总失败" }
        $summaries += $summary
    }
    & $python "$PSScriptRoot\compare_profiles.py" $sweepDir @summaries
    if ($LASTEXITCODE -ne 0) { throw '配置对比失败' }
    Write-Output "SWEEP_DIR=$sweepDir"
} finally {
    Pop-Location
}
