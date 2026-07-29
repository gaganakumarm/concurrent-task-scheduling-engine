[CmdletBinding()]
param(
    [switch]$SkipCandidate
)

$ErrorActionPreference = "Stop"
$repositoryRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repositoryRoot

function Invoke-Checked {
    param(
        [Parameter(Mandatory)]
        [string]$Program,
        [Parameter(ValueFromRemainingArguments)]
        [string[]]$Arguments
    )

    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE"
    }
}

try {
    Invoke-Checked cmake -S . -B build -G "MinGW Makefiles"
    Invoke-Checked cmake --build build
    Invoke-Checked ctest --test-dir build -N
    Invoke-Checked ctest --test-dir build --output-on-failure
    Invoke-Checked .\build\concurrent-task-scheduling-engine.exe

    Invoke-Checked cmake -S . -B build-bench -G "MinGW Makefiles" `
        -DCMAKE_BUILD_TYPE=Release
    Invoke-Checked cmake --build build-bench
    Invoke-Checked .\build-bench\concurrent_scheduler_benchmarks.exe --self-test

    Invoke-Checked cmake -S . -B build-profile -G "MinGW Makefiles" `
        -DCMAKE_BUILD_TYPE=Release `
        -DCONCURRENT_SCHEDULER_ENABLE_PROFILING=ON
    Invoke-Checked cmake --build build-profile
    Invoke-Checked .\build-profile\concurrent_scheduler_benchmarks.exe --self-test

    if (-not $SkipCandidate) {
        Invoke-Checked cmake -S . -B build-opt-candidate `
            -G "MinGW Makefiles" `
            -DCMAKE_BUILD_TYPE=Release `
            -DCONCURRENT_SCHEDULER_ENABLE_PROFILING=ON `
            -DCONCURRENT_SCHEDULER_USE_TRANSITION_SIGNALING=ON
        Invoke-Checked cmake --build build-opt-candidate
        Invoke-Checked `
            .\build-opt-candidate\concurrent_scheduler_benchmarks.exe `
            --self-test
    }

    Write-Host "PHASE 5 REPRODUCTION VALIDATION PASSED"
} catch {
    Write-Error "PHASE 5 REPRODUCTION VALIDATION FAILED: $_"
    exit 1
}
