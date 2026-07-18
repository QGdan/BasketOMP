param(
    [ValidateSet('small', 'medium', 'large')]
    [string]$Dataset = 'medium',
    [int]$Repeats = 3,
    [int]$SerialRepeats = 0,
    [int]$Warmups = 1,
    [int[]]$ThreadCounts = @(1, 2, 4, 8),
    [switch]$AutoThreadCounts,
    [int]$MaxThreads = 0,
    [switch]$AllowOversubscription,
    [switch]$RandomizeThreadOrder,
    [ValidateSet('static', 'dynamic', 'guided')]
    [string]$CooccurSchedule = 'dynamic',
    [int]$CooccurChunk = 64,
    [ValidateSet('static', 'dynamic', 'guided')]
    [string]$RecommendSchedule = 'dynamic',
    [int]$RecommendChunk = 16,
    [int]$TopK = 10,
    [int]$MaxNeighbors = 0,
    [string]$OutputRoot = 'results\experiments'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

function Convert-KeyValueOutput([string[]]$Lines) {
    $values = @{}
    foreach ($line in $Lines) {
        $parts = $line -split '=', 2
        if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] }
    }
    return $values
}

function Get-RequiredValue([hashtable]$Values, [string]$Name) {
    if (-not $Values.ContainsKey($Name)) {
        throw "program output is missing required field: $Name"
    }
    return $Values[$Name]
}

function Invoke-Program([string]$Mode, [int]$Threads) {
    $arguments = @('--data', "data\$Dataset", '--mode', $Mode,
                   '--threads', $Threads, '--top-k', $TopK,
                   '--max-neighbors', $MaxNeighbors,
                   '--cooccur-schedule', $CooccurSchedule,
                   '--cooccur-chunk', $CooccurChunk,
                   '--recommend-schedule', $RecommendSchedule,
                   '--recommend-chunk', $RecommendChunk)
    $output = & '.\build\basket_recommender.exe' @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "benchmark failed: mode=$Mode threads=$Threads"
    }
    return Convert-KeyValueOutput $output
}

function New-BenchmarkRecord([string]$Mode, [int]$Threads, [int]$Repeat,
                             [hashtable]$Value) {
    [pscustomobject][ordered]@{
        run_id = $script:runId
        timestamp = (Get-Date).ToString('o')
        dataset = $Dataset
        algorithm_profile = Get-RequiredValue $Value 'algorithm_profile'
        version = $Mode
        mode = $Mode
        threads = if ($Mode -eq 'serial') { 1 } else { $Threads }
        repeat = $Repeat
        run = $Repeat
        warmup = 0
        top_k = $TopK
        max_neighbors = $MaxNeighbors
        popular_fallback = 0
        schedule = if ($Mode -eq 'serial') { 'none' } else { $CooccurSchedule }
        cooccur_schedule = if ($Mode -eq 'serial') { 'none' } else { $CooccurSchedule }
        cooccur_chunk = $CooccurChunk
        recommend_schedule = if ($Mode -eq 'serial') { 'none' } else { $RecommendSchedule }
        recommend_chunk = $RecommendChunk
        orders = [uint64](Get-RequiredValue $Value 'orders')
        prior_rows = [uint64](Get-RequiredValue $Value 'prior_rows')
        train_rows = [uint64](Get-RequiredValue $Value 'train_rows')
        products = [uint32](Get-RequiredValue $Value 'products')
        users = [uint32](Get-RequiredValue $Value 'users')
        hardware_threads = [uint32](Get-RequiredValue $Value 'hardware_threads')
        omp_max_threads = [uint32](Get-RequiredValue $Value 'omp_max_threads')
        unique_pairs = [uint64](Get-RequiredValue $Value 'unique_pairs')
        pair_events = [uint64](Get-RequiredValue $Value 'pair_events')
        graph_edge_entries = [uint64](Get-RequiredValue $Value 'graph_edge_entries')
        max_degree = [uint32](Get-RequiredValue $Value 'max_degree')
        active_users = [uint32](Get-RequiredValue $Value 'active_users')
        candidate_shortage_users = [uint32](Get-RequiredValue $Value 'candidate_shortage_users')
        empty_candidate_users = [uint32](Get-RequiredValue $Value 'empty_candidate_users')
        total_candidates = [uint64](Get-RequiredValue $Value 'total_candidates')
        max_candidates = [uint32](Get-RequiredValue $Value 'max_candidates')
        load_ms = [double](Get-RequiredValue $Value 'load_ms')
        cooccur_compute_ms = [double](Get-RequiredValue $Value 'cooccur_compute_ms')
        merge_ms = [double](Get-RequiredValue $Value 'merge_ms')
        normalization_ms = [double](Get-RequiredValue $Value 'normalization_ms')
        adjacency_ms = [double](Get-RequiredValue $Value 'adjacency_ms')
        truncate_ms = [double](Get-RequiredValue $Value 'truncate_ms')
        recommend_ms = [double](Get-RequiredValue $Value 'recommend_ms')
        evaluate_ms = [double](Get-RequiredValue $Value 'evaluate_ms')
        algorithm_ms = [double](Get-RequiredValue $Value 'algorithm_ms')
        end_to_end_ms = [double](Get-RequiredValue $Value 'end_to_end_ms')
        hit_rate = [double](Get-RequiredValue $Value 'hit_rate')
        precision = [double](Get-RequiredValue $Value 'precision')
        recall = [double](Get-RequiredValue $Value 'recall')
        f1 = [double](Get-RequiredValue $Value 'f1')
        ndcg = [double](Get-RequiredValue $Value 'ndcg')
        mrr = [double](Get-RequiredValue $Value 'mrr')
        micro_precision = [double](Get-RequiredValue $Value 'micro_precision')
        micro_recall = [double](Get-RequiredValue $Value 'micro_recall')
        cooccur_checksum = Get-RequiredValue $Value 'cooccur_checksum'
        recommendation_checksum = Get-RequiredValue $Value 'recommendation_checksum'
        status = 'ok'
    }
}

if ($Repeats -lt 1 -or $SerialRepeats -lt 0 -or $Warmups -lt 0 -or
    $MaxThreads -lt 0 -or $TopK -lt 1 -or
    $MaxNeighbors -lt 0 -or $CooccurChunk -lt 1 -or $RecommendChunk -lt 1) {
    throw 'Repeats/TopK/chunks must be positive; Warmups/MaxNeighbors cannot be negative'
}
if ($AutoThreadCounts) {
    $limit = if ($MaxThreads -gt 0) { $MaxThreads } else { [Environment]::ProcessorCount }
    $generated = @(1)
    $value = 2
    while ($value -lt $limit) {
        $generated += $value
        $value *= 2
    }
    if ($generated[-1] -ne $limit) { $generated += $limit }
    # Add quarter/half-capacity points to reveal core and NUMA boundaries.
    if ($limit -ge 16) {
        $generated += [int]($limit / 4)
        $generated += [int]($limit / 2)
    }
    $ThreadCounts = $generated
}
$ThreadCounts = @($ThreadCounts | Sort-Object -Unique)
if ($ThreadCounts.Count -eq 0 -or ($ThreadCounts | Where-Object { $_ -lt 1 })) {
    throw 'ThreadCounts must contain only positive integers'
}
if ($MaxThreads -gt 0 -and ($ThreadCounts | Where-Object { $_ -gt $MaxThreads })) {
    throw 'ThreadCounts cannot exceed MaxThreads'
}
$detectedThreads = [Environment]::ProcessorCount
if (-not $AllowOversubscription -and
    ($ThreadCounts | Where-Object { $_ -gt $detectedThreads })) {
    Write-Warning "requested thread count exceeds locally detected $detectedThreads logical processors; use -AllowOversubscription to silence this warning"
}
if ($SerialRepeats -eq 0) { $SerialRepeats = $Repeats }

Push-Location $projectRoot
try {
    & "$PSScriptRoot\build.ps1" Release
    $profile = if ($MaxNeighbors -eq 0) { 'fast-normalization-full' } else { "fast-normalization-top$MaxNeighbors" }
    $runId = "$(Get-Date -Format 'yyyyMMdd-HHmmssfff')-$Dataset-$profile"
    $experimentDir = Join-Path $OutputRoot $runId
    New-Item -ItemType Directory -Force -Path $experimentDir | Out-Null

    for ($warmup = 1; $warmup -le $Warmups; ++$warmup) {
        Invoke-Program 'serial' 1 | Out-Null
        foreach ($threads in $ThreadCounts) { Invoke-Program 'openmp' $threads | Out-Null }
    }

    $records = @()
    for ($repeat = 1; $repeat -le $SerialRepeats; ++$repeat) {
        $serialValue = Invoke-Program 'serial' 1
        $records += New-BenchmarkRecord 'serial' 1 $repeat $serialValue
    }
    for ($repeat = 1; $repeat -le $Repeats; ++$repeat) {
        $orderedThreads = $ThreadCounts
        if ($RandomizeThreadOrder) {
            $orderedThreads = @($ThreadCounts | Sort-Object { Get-Random })
        }
        foreach ($threads in $orderedThreads) {
            $parallelValue = Invoke-Program 'openmp' $threads
            $records += New-BenchmarkRecord 'openmp' $threads $repeat $parallelValue
        }
    }

    $rawPath = Join-Path $experimentDir 'raw.csv'
    $records | Export-Csv -NoTypeInformation -Encoding UTF8 $rawPath
    & "$PSScriptRoot\collect_environment.ps1" -OutputPath (Join-Path $experimentDir 'manifest.json') -RunId $runId -Dataset $Dataset -Profile $profile -Repeats $Repeats -SerialRepeats $SerialRepeats -Warmups $Warmups -ThreadCounts $ThreadCounts -TopK $TopK -MaxNeighbors $MaxNeighbors -CooccurSchedule $CooccurSchedule -CooccurChunk $CooccurChunk -RecommendSchedule $RecommendSchedule -RecommendChunk $RecommendChunk
    Write-Output $rawPath
} finally {
    Pop-Location
}
