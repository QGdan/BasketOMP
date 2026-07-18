param(
    [Parameter(Mandatory=$true)][string]$OutputPath,
    [Parameter(Mandatory=$true)][string]$RunId,
    [Parameter(Mandatory=$true)][string]$Dataset,
    [Parameter(Mandatory=$true)][string]$Profile,
    [int]$Repeats,
    [int]$SerialRepeats,
    [int]$Warmups,
    [int[]]$ThreadCounts,
    [int]$TopK,
    [int]$MaxNeighbors,
    [string]$CooccurSchedule,
    [int]$CooccurChunk,
    [int]$MergeBuckets,
    [string]$RecommendSchedule,
    [int]$RecommendChunk
)

$ErrorActionPreference = 'Stop'
$cpuName = (Get-ItemProperty -LiteralPath 'HKLM:\HARDWARE\DESCRIPTION\System\CentralProcessor\0' -ErrorAction SilentlyContinue).ProcessorNameString
$logicalProcessors = [Environment]::ProcessorCount
$physicalCores = $null
$memoryGiB = $null
$osCaption = [Environment]::OSVersion.VersionString
$osVersion = [Environment]::OSVersion.Version.ToString()
try {
    $cpu = Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object -First 1
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
    $cpuName = $cpu.Name
    $physicalCores = $cpu.NumberOfCores
    $logicalProcessors = $cpu.NumberOfLogicalProcessors
    $memoryGiB = [math]::Round($os.TotalVisibleMemorySize / 1MB, 2)
    $osCaption = $os.Caption
    $osVersion = $os.Version
} catch {
    # 受限环境可能禁止 CIM；保留可由 .NET/注册表安全读取的字段。
    $null = $_
}
$compiler = (& gcc --version | Select-Object -First 1)
$manifest = [ordered]@{
    schema_version = 1
    run_id = $RunId
    created_at = (Get-Date).ToString('o')
    dataset = $Dataset
    algorithm_profile = $Profile
    executable = 'build/basket_recommender.exe'
    build_type = 'Release'
    compiler = $compiler
    compile_flags = '-O2 -DNDEBUG -std=c11 -Wall -Wextra -Wpedantic -fopenmp -lm'
    environment = [ordered]@{
        os = $osCaption
        os_version = $osVersion
        cpu = $cpuName
        physical_cores = $physicalCores
        logical_processors = $logicalProcessors
        memory_gib = $memoryGiB
    }
    parameters = [ordered]@{
        repeats = $Repeats
        serial_repeats = $SerialRepeats
        warmups = $Warmups
        thread_counts = $ThreadCounts
        top_k = $TopK
        max_neighbors = $MaxNeighbors
        cooccur_schedule = $CooccurSchedule
        cooccur_chunk = $CooccurChunk
        merge_buckets = $MergeBuckets
        recommend_schedule = $RecommendSchedule
        recommend_chunk = $RecommendChunk
    }
    files = [ordered]@{
        raw = 'raw.csv'
        validation = 'validation.json'
        summary = 'summary.csv'
        figures = 'figures/'
    }
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
