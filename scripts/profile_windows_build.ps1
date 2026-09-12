# Preserve the production build order and parallelism; collect evidence only.
$ErrorActionPreference = 'Stop'
$profileDir = New-Item -ItemType Directory -Force -Path 'build/profile'
$profilePath = $profileDir.FullName
Get-CimInstance Win32_Processor |
    Select-Object Name, NumberOfCores, NumberOfLogicalProcessors |
    ConvertTo-Json | Set-Content "$profilePath/cpu.json"
Get-CimInstance Win32_OperatingSystem |
    Select-Object Caption, Version, TotalVisibleMemorySize |
    ConvertTo-Json | Set-Content "$profilePath/os.json"
Copy-Item build/CMakeCache.txt "$profilePath/CMakeCache.txt"

$sampler = Start-Job -ArgumentList $profilePath -ScriptBlock {
    param($OutputPath)
    try {
        while ($true) {
            $cpu = Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'"
            $memory = Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory
            $compilers = @(Get-Process cl -ErrorAction SilentlyContinue)
            [pscustomobject]@{
                utc = [DateTime]::UtcNow.ToString('o')
                cpu_percent = $cpu.PercentProcessorTime
                available_mb = $memory.AvailableMBytes
                pages_input_per_sec = $memory.PagesInputPersec
                compiler_processes = $compilers.Count
                compiler_working_set_bytes = ($compilers | Measure-Object WorkingSet64 -Sum).Sum
            } | Export-Csv "$OutputPath/resources.csv" -Append -NoTypeInformation
            Start-Sleep -Seconds 5
        }
    } catch {
        $_ | Out-String | Set-Content "$OutputPath/sampler-error.txt"
    }
}

try {
    foreach ($target in @('lubancode', 'lubancode_tests')) {
        $clock = [Diagnostics.Stopwatch]::StartNew()
        # Disable embedded project imports; binlogs remain diagnostic artifacts.
        & cmake --build build --config Release --target $target -j 4 -- "/bl:$profilePath/$target.binlog;ProjectImports=None" /clp:PerformanceSummary
        $buildExit = $LASTEXITCODE
        $clock.Stop()
        [pscustomobject]@{
            target = $target
            seconds = $clock.Elapsed.TotalSeconds
            exit_code = $buildExit
        } | Export-Csv "$profilePath/timings.csv" -Append -NoTypeInformation
        if ($buildExit -ne 0) { throw "Build failed: $target (exit $buildExit)" }
    }
} finally {
    Stop-Job $sampler
    Receive-Job $sampler | Out-String | Set-Content "$profilePath/sampler-output.txt"
    Remove-Job $sampler
}
