param(
    [ValidateSet('small', 'medium', 'large')]
    [string]$Dataset = 'medium',

    [int]$Repeats = 5,
    [int]$SerialRepeats = 3,
    [int]$Warmups = 1,

    [int[]]$ThreadCounts = @(),
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

    # ── OpenMP 线程绑定 ──────────────────
    [ValidateSet('spread', 'close', 'master')]
    [string]$ProcBind = 'spread',
    [ValidateSet('cores', 'threads', 'sockets')]
    [string]$Places = 'cores',

    # ── 后处理控制 ────────────────────────
    [switch]$NoPostProcess,
    [switch]$NoPlot,

    [string]$OutputRoot = 'results\experiments'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

# 检测 Python（用于自动化后处理）
$python = $null
if (-not $NoPostProcess) {
    $candidates = @('python', 'python3', 'py')
    foreach ($cmd in $candidates) {
        $found = Get-Command $cmd -ErrorAction SilentlyContinue
        if ($found) {
            $python = $found.Source
            break
        }
    }
    if (-not $python) {
        Write-Warning '未找到 Python，将跳过自动后处理。使用 -NoPostProcess 可消除此警告。'
    }
}

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
        run_id          = $script:runId
        timestamp       = (Get-Date).ToString('o')
        dataset         = $Dataset
        algorithm_profile = Get-RequiredValue $Value 'algorithm_profile'
        version         = $Mode
        mode            = $Mode
        threads         = if ($Mode -eq 'serial') { 1 } else { $Threads }
        repeat          = $Repeat
        run             = $Repeat
        warmup          = 0
        top_k           = $TopK
        max_neighbors   = $MaxNeighbors
        popular_fallback = 0
        schedule        = if ($Mode -eq 'serial') { 'none' } else { $CooccurSchedule }
        cooccur_schedule = if ($Mode -eq 'serial') { 'none' } else { $CooccurSchedule }
        cooccur_chunk   = $CooccurChunk
        recommend_schedule = if ($Mode -eq 'serial') { 'none' } else { $RecommendSchedule }
        recommend_chunk = $RecommendChunk
        orders          = [uint64](Get-RequiredValue $Value 'orders')
        prior_rows      = [uint64](Get-RequiredValue $Value 'prior_rows')
        train_rows      = [uint64](Get-RequiredValue $Value 'train_rows')
        products        = [uint32](Get-RequiredValue $Value 'products')
        users           = [uint32](Get-RequiredValue $Value 'users')
        hardware_threads = [uint32](Get-RequiredValue $Value 'hardware_threads')
        omp_max_threads  = [uint32](Get-RequiredValue $Value 'omp_max_threads')
        unique_pairs    = [uint64](Get-RequiredValue $Value 'unique_pairs')
        pair_events     = [uint64](Get-RequiredValue $Value 'pair_events')
        graph_edge_entries = [uint64](Get-RequiredValue $Value 'graph_edge_entries')
        max_degree      = [uint32](Get-RequiredValue $Value 'max_degree')
        active_users    = [uint32](Get-RequiredValue $Value 'active_users')
        candidate_shortage_users = [uint32](Get-RequiredValue $Value 'candidate_shortage_users')
        empty_candidate_users = [uint32](Get-RequiredValue $Value 'empty_candidate_users')
        total_candidates = [uint64](Get-RequiredValue $Value 'total_candidates')
        max_candidates  = [uint32](Get-RequiredValue $Value 'max_candidates')
        load_ms         = [double](Get-RequiredValue $Value 'load_ms')
        cooccur_compute_ms = [double](Get-RequiredValue $Value 'cooccur_compute_ms')
        merge_ms        = [double](Get-RequiredValue $Value 'merge_ms')
        normalization_ms = [double](Get-RequiredValue $Value 'normalization_ms')
        adjacency_ms    = [double](Get-RequiredValue $Value 'adjacency_ms')
        truncate_ms     = [double](Get-RequiredValue $Value 'truncate_ms')
        recommend_ms    = [double](Get-RequiredValue $Value 'recommend_ms')
        evaluate_ms     = [double](Get-RequiredValue $Value 'evaluate_ms')
        algorithm_ms    = [double](Get-RequiredValue $Value 'algorithm_ms')
        end_to_end_ms   = [double](Get-RequiredValue $Value 'end_to_end_ms')
        hit_rate        = [double](Get-RequiredValue $Value 'hit_rate')
        precision       = [double](Get-RequiredValue $Value 'precision')
        recall          = [double](Get-RequiredValue $Value 'recall')
        f1              = [double](Get-RequiredValue $Value 'f1')
        ndcg            = [double](Get-RequiredValue $Value 'ndcg')
        mrr             = [double](Get-RequiredValue $Value 'mrr')
        micro_precision  = [double](Get-RequiredValue $Value 'micro_precision')
        micro_recall    = [double](Get-RequiredValue $Value 'micro_recall')
        cooccur_checksum = Get-RequiredValue $Value 'cooccur_checksum'
        recommendation_checksum = Get-RequiredValue $Value 'recommendation_checksum'
        status          = 'ok'
    }
}

# ── 参数校验 ────────────────────────────
if ($Repeats -lt 1 -or $SerialRepeats -lt 0 -or $Warmups -lt 0 -or
    $MaxThreads -lt 0 -or $TopK -lt 1 -or
    $MaxNeighbors -lt 0 -or $CooccurChunk -lt 1 -or $RecommendChunk -lt 1) {
    throw 'Repeats/TopK/chunks 必须为正数；Warmups/MaxNeighbors 不可为负'
}

# ── 自动线程数生成 ──────────────────────
$detectedThreads = [Environment]::ProcessorCount
if ($AutoThreadCounts -or $ThreadCounts.Count -eq 0) {
    $limit = if ($MaxThreads -gt 0) { $MaxThreads } else { $detectedThreads }
    $generated = @(1)
    $value = 2
    while ($value -lt $limit) {
        $generated += $value
        $value *= 2
    }
    if ($generated[-1] -ne $limit) { $generated += $limit }
    # 四分之一和半容量点揭示核心和NUMA边界
    if ($limit -ge 16) {
        $generated += [int]($limit / 4)
        $generated += [int]($limit / 2)
    }
    $ThreadCounts = $generated
}
$ThreadCounts = @($ThreadCounts | Sort-Object -Unique)
if ($ThreadCounts.Count -eq 0 -or ($ThreadCounts | Where-Object { $_ -lt 1 })) {
    throw 'ThreadCounts 必须只包含正整数'
}
if ($MaxThreads -gt 0 -and ($ThreadCounts | Where-Object { $_ -gt $MaxThreads })) {
    throw 'ThreadCounts 不能超过 MaxThreads'
}
if (-not $AllowOversubscription -and
    ($ThreadCounts | Where-Object { $_ -gt $detectedThreads })) {
    Write-Warning "请求的线程数 ($($ThreadCounts -join ',')) 超过本地检测到的 $detectedThreads 个逻辑处理器；使用 -AllowOversubscription 可关闭此警告"
}
if ($SerialRepeats -eq 0) { $SerialRepeats = $Repeats }

# ── OpenMP 环境变量 ────────────────────
$origProcBind = $env:OMP_PROC_BIND
$origPlaces = $env:OMP_PLACES
try {
    $env:OMP_PROC_BIND = $ProcBind
    $env:OMP_PLACES = $Places
    Write-Output "OMP_PROC_BIND=$ProcBind  OMP_PLACES=$Places"

    Push-Location $projectRoot
    try {
        & "$PSScriptRoot\build.ps1" Release

        $profile = if ($MaxNeighbors -eq 0) { 'fast-normalization-full' } else { "fast-normalization-top$MaxNeighbors" }
        $runId = "$(Get-Date -Format 'yyyyMMdd-HHmmssfff')-$Dataset-$profile"
        $experimentDir = Join-Path $OutputRoot $runId
        New-Item -ItemType Directory -Force -Path $experimentDir | Out-Null

        # ── 预热 ───────────────────────────
        for ($warmup = 1; $warmup -le $Warmups; ++$warmup) {
            Invoke-Program 'serial' 1 | Out-Null
            foreach ($threads in $ThreadCounts) { Invoke-Program 'openmp' $threads | Out-Null }
        }

        # ── 计时运行 ──────────────────────
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

        # ── 保存 raw CSV ──────────────────
        $rawPath = Join-Path $experimentDir 'raw.csv'
        $records | Export-Csv -NoTypeInformation -Encoding UTF8 $rawPath

        # ── 写入 manifest ─────────────────
        & "$PSScriptRoot\collect_environment.ps1" `
            -OutputPath (Join-Path $experimentDir 'manifest.json') `
            -RunId $runId -Dataset $Dataset -Profile $profile `
            -Repeats $Repeats -SerialRepeats $SerialRepeats -Warmups $Warmups `
            -ThreadCounts $ThreadCounts -TopK $TopK -MaxNeighbors $MaxNeighbors `
            -CooccurSchedule $CooccurSchedule -CooccurChunk $CooccurChunk `
            -RecommendSchedule $RecommendSchedule -RecommendChunk $RecommendChunk

        # ── 自动化后处理 ──────────────────
        if (-not $NoPostProcess -and $python) {
            Write-Output "`n── 自动后处理 ──────────────────────────"

            $validationPath = Join-Path $experimentDir 'validation.json'
            & $python "$PSScriptRoot\validate_results.py" $rawPath $validationPath
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "验证失败！请检查 $validationPath"
            } else {
                Write-Output "  ✓ 验证通过"
            }

            $summaryPath = Join-Path $experimentDir 'summary.csv'
            & $python "$PSScriptRoot\summarize_results.py" $rawPath $summaryPath
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "汇总生成失败！"
            } else {
                Write-Output "  ✓ 汇总完成"
            }

            if (-not $NoPlot) {
                $figuresDir = Join-Path $experimentDir 'figures'
                & $python "$PSScriptRoot\plot_results.py" $summaryPath $figuresDir
                if ($LASTEXITCODE -ne 0) {
                    Write-Warning "图表生成失败！"
                } else {
                    Write-Output "  ✓ 图表已生成"
                }
            }
        }

        # ── 输出结构化结果摘要 ────────────
        Write-Output "`n── 实验完成 ────────────────────────────"
        Write-Output "EXPERIMENT_DIR=$experimentDir"
        Write-Output "RAW_CSV=$rawPath"
        Write-Output "DATASET=$Dataset"
        Write-Output "PROFILE=$profile"
        Write-Output "THREADS=$($ThreadCounts -join ',')"
        Write-Output "REPEATS=$Repeats"
    } finally {
        Pop-Location
    }
} finally {
    # 恢复环境变量
    if ($origProcBind) { $env:OMP_PROC_BIND = $origProcBind } else { Remove-Item env:OMP_PROC_BIND -ErrorAction SilentlyContinue }
    if ($origPlaces)   { $env:OMP_PLACES   = $origPlaces   } else { Remove-Item env:OMP_PLACES   -ErrorAction SilentlyContinue }
}
