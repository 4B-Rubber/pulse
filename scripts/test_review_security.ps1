param([int]$WaitForBuildPid = 0)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$run = Join-Path $root ('bench_data\review-security-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $run | Out-Null
$failed = $false
try {
    if ($WaitForBuildPid -and (Get-Process -Id $WaitForBuildPid -ErrorAction SilentlyContinue)) {
        Wait-Process -Id $WaitForBuildPid -Timeout 1800
    }
    & cmd.exe /d /c "call scripts\vcvars.bat && cmake --build build_st --target pulse pulse_review_security_test pulse_network_agent_client_test pulse_index_migration_test -j 4 > `"$run\build.log`" 2>&1"
    if ($LASTEXITCODE) { throw 'Security targets did not build; see build.log' }
    foreach ($name in @('pulse_review_security_test','pulse_network_agent_client_test','pulse_index_migration_test')) {
        $p = New-Object System.Diagnostics.Process
        $p.StartInfo.FileName = "$root\build_st\$name.exe"
        $p.StartInfo.WorkingDirectory = $root
        $p.StartInfo.UseShellExecute = $false
        $p.StartInfo.CreateNoWindow = $true
        $p.StartInfo.RedirectStandardOutput = $true
        $p.StartInfo.RedirectStandardError = $true
        $null = $p.Start()
        $stdout = $p.StandardOutput.ReadToEndAsync()
        $stderr = $p.StandardError.ReadToEndAsync()
        if (!$p.WaitForExit(45000)) {
            $p.Kill()
            throw "Fixture $name timed out; only PID $($p.Id) stopped"
        }
        $code = $p.ExitCode
        [IO.File]::WriteAllText("$run\$name.log", $stdout.Result)
        [IO.File]::WriteAllText("$run\$name.err", $stderr.Result)
        Write-Output $stdout.Result
        Write-Output $stderr.Result
        Write-Output "$name EXIT=$code"
        if ($code -ne 0) { $failed = $true }
        $p.Dispose()
    }
} catch { Write-Output "[FAIL] $($_.Exception.Message)"; $failed = $true }
[IO.File]::WriteAllText("$run\exit.txt", [string][int]$failed)
Write-Output "RESULT_DIR=$run"
if ($failed) { exit 1 }
