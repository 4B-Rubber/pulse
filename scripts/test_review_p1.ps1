param([string]$BuildDir = 'build_st', [int]$WaitForBuildPid = 0, [switch]$Rebuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$run = Join-Path $root ('bench_data\review-p1-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $run | Out-Null
$summary = Join-Path $run 'summary.txt'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$failures = 0
function Check([bool]$ok, [string]$name) {
    $line = if ($ok) { "[PASS] $name" } else { "[FAIL] $name" }
    $line | Tee-Object -FilePath $summary -Append
    if (!$ok) { $script:failures++ }
}
function RunFixture([string[]]$arguments) {
    if ($arguments.Count) {
        $process = Start-Process -FilePath (Join-Path $root "$BuildDir\pulse.exe") -ArgumentList $arguments -PassThru
    } else {
        $process = Start-Process -FilePath (Join-Path $root "$BuildDir\pulse.exe") -PassThru
    }
    if (!$process.WaitForExit(45000)) {
        $process.Kill()
        throw "Timed out: only fixture PID $($process.Id) was terminated"
    }
    $process.Refresh()
    return $process.ExitCode
}
try {
    if ($WaitForBuildPid -gt 0 -and (Get-Process -Id $WaitForBuildPid -ErrorAction SilentlyContinue)) {
        Wait-Process -Id $WaitForBuildPid -Timeout 1800
    }
    if ($Rebuild) {
        $buildLog = Join-Path $run 'build.log'
        & cmd.exe /d /c "call scripts\vcvars.bat && set VSLANG=1033&& cmake --build $BuildDir --target pulse -j 4 > `"$buildLog`" 2>&1"
        if ($LASTEXITCODE -ne 0) { throw "Build failed; see $buildLog" }
    }
    $cache = Get-Content "$BuildDir\CMakeCache.txt" -Raw
    if ($cache -notmatch 'PULSE_WITH_SELFTEST:BOOL=ON') { throw 'Use a selftest-enabled build; never run these fixtures against production data paths' }
    foreach ($stage in @('graphics', 'after-graphics')) {
        $data = Join-Path $run $stage
        New-Item -ItemType Directory -Path $data | Out-Null
        $fixtures = @{
            'app.json' = '{"theme":"dark","confirmDelete":true,"fixture":"preserve-app"}'
            'context_menu.json' = '{"fixture":"preserve-menu","hidden":["test-only"]}'
            'places.json' = '{"fixture":"preserve-places","networks":["\\\\fixture-invalid\\share"]}'
            'session.json' = '{"path":"C:\\P1-Fixture","activeTab":1,"layoutTabs":[{"title":"one","layout":1,"panes":[{"path":"C:\\P1-Fixture-A","view":"details"},{"path":"C:\\P1-Fixture-B","view":"details"}]},{"title":"two","layout":0,"panes":[{"path":"C:\\P1-Fixture-C","view":"details"}]}],"undo":[{"fixture":"preserve-undo"}]}'
        }
        $before = @{}
        foreach ($name in $fixtures.Keys) {
            $file = Join-Path $data $name
            [IO.File]::WriteAllText($file, $fixtures[$name], $utf8)
            $before[$name] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
        }
        $env:PULSE_TEST_DATA_DIR = $data
        $env:PULSE_TEST_STARTUP_FAIL = $stage
        # Intentionally no --test-instance/--shot: exercise normal exit persistence.
        $code = RunFixture @()
        Check ($code -eq 1) "M18 $stage startup exits with failure"
        $marker = Join-Path $data 'startup-rollback.txt'
        Check ((Test-Path $marker) -and ((Get-Content $marker -Raw) -match 'WM_DESTROY startupComplete=0 isolatedTest=0')) "M18 $stage actually executes normal-instance rollback"
        foreach ($name in $fixtures.Keys) {
            Check ((Get-FileHash (Join-Path $data $name) -Algorithm SHA256).Hash -eq $before[$name]) "M18 $stage preserves $name byte-for-byte"
        }
    }
    Remove-Item Env:PULSE_TEST_STARTUP_FAIL
    $data = Join-Path $run 'interaction-data'
    $files = Join-Path $run 'fixture-files'
    New-Item -ItemType Directory -Path $data,$files | Out-Null
    foreach ($name in @('a.keep','b.secret','c.keep')) { [IO.File]::WriteAllText((Join-Path $files $name), 'fixture only', $utf8) }
    $env:PULSE_TEST_DATA_DIR = $data
    $env:PULSE_TEST_P1_FLOW = Join-Path $run 'interaction.txt'
    $code = RunFixture @('--test-instance','--shot', ('"' + (Join-Path $run 'unused.png') + '"'), ('"' + $files + '"'))
    Check ($code -eq 0) 'M10/M14/M18 production GUI/model/controller probe exits successfully'
    if (Test-Path $env:PULSE_TEST_P1_FLOW) {
        Get-Content $env:PULSE_TEST_P1_FLOW | Tee-Object -FilePath $summary -Append
        if (Select-String -Path $env:PULSE_TEST_P1_FLOW -Pattern '^\[FAIL\]' -Quiet) { $failures++ }
    } else { Check $false 'interaction probe log exists' }
    Check (Test-Path (Join-Path $data 'session.json')) 'M18 successful update handshake wrote isolated session'
    Check ((Get-ChildItem $files -File).Count -eq 3) 'fixture files remain; no delete/move dispatched'
} catch {
    Check $false $_.Exception.Message
} finally {
    Remove-Item Env:PULSE_TEST_DATA_DIR,Env:PULSE_TEST_STARTUP_FAIL,Env:PULSE_TEST_P1_FLOW -ErrorAction SilentlyContinue
    [IO.File]::WriteAllText((Join-Path $run 'exit.txt'), [string][int]($failures -ne 0), $utf8)
    Write-Output "RESULT_DIR=$run"
}
if ($failures) { exit 1 }