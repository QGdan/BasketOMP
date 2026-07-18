param(
    [ValidateSet('Debug', 'Release', 'Smoke', 'TestLoader', 'TestHash', 'TestCooccur', 'TestRecommender', 'TestEvaluator', 'TestIntegration')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $projectRoot 'build'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$common = @('-std=c11', '-Wall', '-Wextra', '-Wpedantic',
            '-D__USE_MINGW_ANSI_STDIO=1', '-fopenmp', '-Iinclude')
$appSources = @('src/main.c', 'src/csv_loader.c', 'src/pair_hash.c',
                'src/cooccurrence_serial.c', 'src/cooccurrence_openmp.c',
                'src/recommender_serial.c', 'src/recommender_openmp.c',
                'src/evaluator.c')

Push-Location $projectRoot
try {
    if ($Configuration -eq 'Debug') {
        & gcc @common '-O0' '-g' @appSources '-o' 'build/basket_recommender_debug.exe' '-lm'
    } elseif ($Configuration -eq 'Release') {
        & gcc @common '-O2' '-DNDEBUG' @appSources '-o' 'build/basket_recommender.exe' '-lm'
    } elseif ($Configuration -eq 'Smoke') {
        & gcc @common '-O0' '-g' 'tests/omp_smoke.c' '-o' 'build/omp_smoke.exe' '-lm'
    } elseif ($Configuration -eq 'TestLoader') {
        & gcc @common '-O0' '-g' 'tests/test_loader.c' 'src/csv_loader.c' '-o' 'build/test_loader.exe' '-lm'
    } elseif ($Configuration -eq 'TestHash') {
        & gcc @common '-O0' '-g' 'tests/test_pair_hash.c' 'src/pair_hash.c' '-o' 'build/test_pair_hash.exe' '-lm'
    } elseif ($Configuration -eq 'TestCooccur') {
        & gcc @common '-O0' '-g' 'tests/test_cooccurrence.c' 'src/csv_loader.c' 'src/pair_hash.c' 'src/cooccurrence_serial.c' 'src/cooccurrence_openmp.c' '-o' 'build/test_cooccurrence.exe' '-lm'
    } elseif ($Configuration -eq 'TestRecommender') {
        & gcc @common '-O0' '-g' 'tests/test_recommender.c' 'src/csv_loader.c' 'src/pair_hash.c' 'src/cooccurrence_serial.c' 'src/cooccurrence_openmp.c' 'src/recommender_serial.c' 'src/recommender_openmp.c' '-o' 'build/test_recommender.exe' '-lm'
    } elseif ($Configuration -eq 'TestEvaluator') {
        & gcc @common '-O0' '-g' 'tests/test_evaluator.c' 'src/csv_loader.c' 'src/pair_hash.c' 'src/cooccurrence_serial.c' 'src/recommender_serial.c' 'src/evaluator.c' '-o' 'build/test_evaluator.exe' '-lm'
    } else {
        & gcc @common '-O0' '-g' 'tests/test_integration.c' 'src/csv_loader.c' 'src/pair_hash.c' 'src/cooccurrence_serial.c' 'src/cooccurrence_openmp.c' 'src/recommender_serial.c' 'src/recommender_openmp.c' '-o' 'build/test_integration.exe' '-lm'
    }

    if ($LASTEXITCODE -ne 0) {
        throw "GCC failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}
