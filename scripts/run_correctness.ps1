$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

# OpenMP 线程绑定（确保测试可重现）
$origProcBind = $env:OMP_PROC_BIND
$origPlaces = $env:OMP_PLACES
$env:OMP_PROC_BIND = 'spread'
$env:OMP_PLACES = 'cores'

function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Executable failed with exit code $LASTEXITCODE"
    }
}

Push-Location $projectRoot
try {
    foreach ($target in @('TestLoader', 'TestHash', 'TestCooccur',
                           'TestRecommender', 'TestEvaluator', 'TestIntegration',
                           'Debug', 'Release')) {
        & "$PSScriptRoot\build.ps1" $target
    }

    Invoke-Checked '.\build\test_loader.exe' @('data\toy')
    Invoke-Checked '.\build\test_loader.exe' @('data\small')
    Invoke-Checked '.\build\test_pair_hash.exe' @()
    Invoke-Checked '.\build\test_cooccurrence.exe' @('data\toy')
    Invoke-Checked '.\build\test_cooccurrence.exe' @('data\small')
    Invoke-Checked '.\build\test_recommender.exe' @('data\toy')
    Invoke-Checked '.\build\test_recommender.exe' @('data\small')
    Invoke-Checked '.\build\test_evaluator.exe' @()
    Invoke-Checked '.\build\test_integration.exe' @('data\toy')
    Invoke-Checked '.\build\test_integration.exe' @('data\small')

    $serial = & '.\build\basket_recommender_debug.exe' --data data\toy --mode serial --top-k 10
    if ($LASTEXITCODE -ne 0) { throw 'serial integration run failed' }
    $parallel = & '.\build\basket_recommender_debug.exe' --data data\toy --mode openmp --threads 4 --top-k 10
    if ($LASTEXITCODE -ne 0) { throw 'OpenMP integration run failed' }

    $serialChecksum = ($serial | Select-String '^recommendation_checksum=').Line
    $parallelChecksum = ($parallel | Select-String '^recommendation_checksum=').Line
    if ($serialChecksum -ne $parallelChecksum) {
        throw "integration checksum mismatch: $serialChecksum vs $parallelChecksum"
    }
    $expectedChecksum = 'recommendation_checksum=6286369242441534757'
    if ($serialChecksum -ne $expectedChecksum) {
        Write-Warning "toy 基线校验和已变更"
        Write-Warning "  预期: $expectedChecksum"
        Write-Warning "  实际: $serialChecksum"
        Write-Warning "如果确认算法改动正确，请更新 run_correctness.ps1 中的 expectedChecksum"
    }

    $limitedSerial = & '.\build\basket_recommender_debug.exe' --data data\toy --mode serial --top-k 10 --max-neighbors 1
    $limitedParallel = & '.\build\basket_recommender_debug.exe' --data data\toy --mode openmp --threads 4 --top-k 10 --max-neighbors 1 --cooccur-schedule guided --cooccur-chunk 4 --recommend-schedule guided --recommend-chunk 2
    if ($LASTEXITCODE -ne 0) { throw 'Top-N integration run failed' }
    $limitedSerialChecksum = ($limitedSerial | Select-String '^recommendation_checksum=').Line
    $limitedParallelChecksum = ($limitedParallel | Select-String '^recommendation_checksum=').Line
    if ($limitedSerialChecksum -ne $limitedParallelChecksum) {
        throw "Top-N checksum mismatch: $limitedSerialChecksum vs $limitedParallelChecksum"
    }

    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & '.\build\basket_recommender_debug.exe' --data data\toy --mode openmp --threads 2 --recommend-chunk 0 2>$null | Out-Null
    $invalidExitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedPreference
    if ($invalidExitCode -eq 0) { throw 'zero recommendation chunk should fail' }
    'PASS: complete correctness regression'
} finally {
    Pop-Location
    if ($origProcBind) { $env:OMP_PROC_BIND = $origProcBind } else { Remove-Item env:OMP_PROC_BIND -ErrorAction SilentlyContinue }
    if ($origPlaces)   { $env:OMP_PLACES   = $origPlaces   } else { Remove-Item env:OMP_PLACES   -ErrorAction SilentlyContinue }
}
