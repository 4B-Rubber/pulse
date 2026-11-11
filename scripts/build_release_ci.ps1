#requires -Version 7.2
param(
    [ValidateSet('windows', 'win81')][string]$Channel = 'windows',
    [string]$BuildDir = 'build-ci',
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
# The pack catalogs must still describe the assets this release will host.
& python (Join-Path $repo 'scripts/test_release_packs.py')
if ($LASTEXITCODE -ne 0) { throw 'Release pack catalog validation failed before compilation' }
# Per-stage wall time in the log; the release bottleneck lives in this script.
$stage = [System.Diagnostics.Stopwatch]::StartNew()
function Write-StageTime([string]$Name) {
    Write-Output ("[timing] {0}: {1:mm\:ss}" -f $Name, $script:stage.Elapsed)
    $script:stage = [System.Diagnostics.Stopwatch]::StartNew()
}
$build = [IO.Path]::GetFullPath((Join-Path $repo $BuildDir))
if (-not $build.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildDir must be inside the repository'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$toolsetEnvironment = ''
$compilerArguments = ''
if (-not $vs) {
    # A local extracted v143 toolset can share the installed Windows SDK.
    $toolset = Join-Path $repo 'tools/win81-toolchain/VC/Tools/MSVC/14.44.35207'
    if (-not (Test-Path "$toolset/bin/Hostx64/x64/cl.exe")) { throw 'Visual Studio 2022 C++ tools are required' }
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'An installed Windows SDK developer environment is required' }
    $toolsetEnvironment = @"
set "PATH=$toolset\bin\Hostx64\x64;%PATH%"
set "INCLUDE=$toolset\include;%WindowsSdkDir%Include\%WindowsSDKVersion%ucrt;%WindowsSdkDir%Include\%WindowsSDKVersion%shared;%WindowsSdkDir%Include\%WindowsSDKVersion%um;%WindowsSdkDir%Include\%WindowsSDKVersion%winrt"
set "LIB=$toolset\lib\x64;%WindowsSdkDir%Lib\%WindowsSDKVersion%ucrt\x64;%WindowsSdkDir%Lib\%WindowsSDKVersion%um\x64"
"@
    $compilerArguments = "-DCMAKE_C_COMPILER=`"$toolset/bin/Hostx64/x64/cl.exe`" -DCMAKE_CXX_COMPILER=`"$toolset/bin/Hostx64/x64/cl.exe`" -DCMAKE_LINKER=`"$toolset/bin/Hostx64/x64/link.exe`""
}
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
# The release tag pins the complete SDK; CI verifies it before configuring CMake.
$sdkRoot = & (Join-Path $PSScriptRoot 'verify_lumatext_sdk.ps1')
Write-StageTime 'toolchain and SDK verification'
# Compiler cache: the workflow installs sccache and warms actions/cache between
# releases. Version labels ride in generated headers, so most commands repeat.
$sccacheLaunch = ''
if (Get-Command sccache -ErrorAction SilentlyContinue) {
    sccache --start-server 2>$null
    $sccacheLaunch = ' -DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache'
    Write-Output '[timing] sccache launcher enabled'
} else {
    Write-Output '[timing] sccache not found: cold compilation'
}
$candidate = if ($Channel -eq 'win81') { 'ON' } else { 'OFF' }
$manifest = if ($Channel -eq 'win81') { 'update-manifest-win81.json' } else { 'update-manifest.json' }
$publicKey = (Get-Content (Join-Path $repo 'cmake/update-public-key.txt') -Raw).Trim()
$version = (Get-Content (Join-Path $repo 'version.txt') -Raw).Trim()
New-Item -ItemType Directory -Path $build -Force | Out-Null
# Release verification is scoped to the changes being shipped. pulse already
# depends on all three packaged hosts; standalone tests need explicit targets.
$testNames = @('pulse_rename_ops_test', 'pulse_child_edit_test', 'pulse_localization_test',
    'pulse_update_test', 'pulse_update_installer_test', 'pulse_update_session_test', 'pulse_app_controllers_test',
    'pulse_change_tracking_polling_test', 'pulse_change_tracking_test', 'pulse_index_delta_replay_test',
    'pulse_runtime_log_test', 'pulse_diagnostics_export_test', 'pulse_shell_command_test', 'pulse_reparse_entry_test',
    'pulse_link_destination_test', 'pulse_link_pill_test', 'pulse_shell_icons_test', 'pulse_network_locations_test',
    'pulse_archive_integrity_test', 'pulse_document_completeness_test', 'pulse_preview_command_test',
    'pulse_image_preview_integrity_test', 'pulse_udf_listing_test', 'pulse_preview_integrity_test',
    'pulse_change_tracking_memory_test', 'pulse_change_feed_memory_test', 'pulse_usn_packet_queue_test',
    'pulse_content_progress_ui_test', 'pulse_operation_presentation_test', 'pulse_column_strip_test', 'pulse_entry_sort_key_test',
    'pulse_file_lock_test', 'pulse_dialogs_test', 'pulse_transfer_rate_test',
    # AI enhancement pack: catalog/install, JSON, broker, tools, MCP over real pipes.
    'pulse_ai_pack_test', 'pulse_ai_json_test', 'pulse_ai_broker_test', 'pulse_ai_tools_test', 'pulse_ai_mcp_test')
$testTargets = (@('pulse', 'pulse_index_engine_test', 'pulse_index_host_stress',
    'pulse_preview_test', 'pulse_preview_handler_probe', 'pulse_playback_controls_test', 'pulse_ops_test') + $testNames) -join ' '
$testConfiguration = '-DPULSE_WITH_SELFTEST=ON -DPULSE_BUILD_PROGRESS_UI_TESTS=ON'
if ($SkipTests) {
    Write-Output 'Quick release: reuse local test results; build production only and retain all payload/pack/compatibility checks.'
    $testTargets = 'pulse'
    $testConfiguration = '-DPULSE_WITH_SELFTEST=OFF -DPULSE_WITH_TEST_HOOKS=OFF -DPULSE_BUILD_PROGRESS_UI_TESTS=OFF'
}
$batch = Join-Path $build 'compile-release.bat'
@"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 1
chcp 65001 >nul
set "VSLANG=1033"
$toolsetEnvironment
cmake -S "$repo" -B "$build" -G Ninja $compilerArguments$sccacheLaunch -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DPULSE_WIN81_CANDIDATE=$candidate $testConfiguration -DPULSE_WITH_LUMATEXT=ON -DLUMATEXT_SOURCE_DIR= -DCMAKE_PREFIX_PATH="$sdkRoot" -DPULSE_UPDATE_MANIFEST_URL="https://github.com/4B-Rubber/pulse/releases/latest/download/$manifest" -DPULSE_UPDATE_PUBLIC_KEY_HEX=$publicKey -DPULSE_VERSION_LABEL=$version-dev
if errorlevel 1 exit /b 1
echo [timing] configure finished at %TIME%
cmake --build "$build" --parallel 4 --target $testTargets
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& $batch
if ($LASTEXITCODE -ne 0) { throw 'Release compilation failed' }
Write-StageTime 'configure and verification build'
if (-not $SkipTests) {
    # A cold verification run costs ~40 minutes, so record every failure and
    # block publication once at the end instead of stopping at the first one.
    $releaseTestFailures = [System.Collections.Generic.List[string]]::new()
    function Record-ReleaseTestFailure([string]$message) {
        $releaseTestFailures.Add($message)
        Write-Output "[FAIL] $message"
    }
    # Every run keeps its console output in the build folder, which already rides
    # in the diagnostics artifact, and repeats its [FAIL] lines as annotations:
    # a red gate must be diagnosable without the web log viewer.
    function Invoke-ReleaseTest([string]$label, [string]$exe, [string]$failure, [string[]]$arguments = @()) {
        $logPath = Join-Path $build "$label.log"
        & $exe @arguments 2>&1 | Tee-Object -FilePath $logPath
        if ($LASTEXITCODE -eq 0) { return }
        Record-ReleaseTestFailure $failure
        Get-Content -LiteralPath $logPath -ErrorAction SilentlyContinue |
            Select-String '\[FAIL\]' | Select-Object -First 40 |
            ForEach-Object { Write-Output "::error::$($_.Line.Trim())" }
    }
    foreach ($testName in $testNames) {
        Invoke-ReleaseTest $testName (Join-Path $build "$testName.exe") "$testName failed"
    }
    Invoke-ReleaseTest 'ops-update-shutdown' (Join-Path $build 'pulse_ops_test.exe') `
        'Update shutdown and automatic wait regression failed' @('--update-shutdown')
    Invoke-ReleaseTest 'ops-delete-progress' (Join-Path $build 'pulse_ops_test.exe') `
        'Delete progress rate and remaining-time regression failed' @('--delete-progress-only')
    Invoke-ReleaseTest 'playback-timeline' (Join-Path $build 'pulse_playback_controls_test.exe') `
        'Playback timeline regression failed' @('--timeline-only')
    Invoke-ReleaseTest 'preview-vector' (Join-Path $build 'pulse_preview_test.exe') `
        'Vector preview regression failed' @('--vector-only')
    Invoke-ReleaseTest 'preview-cooldown' (Join-Path $build 'pulse_preview_handler_probe.exe') `
        'Preview provider cooldown regression failed' @('--cooldown-test')
    foreach ($mode in @('--startup-stop', '--shell-roundtrip')) {
        $label = 'rename' + ($mode -replace '^--', '-')
        Invoke-ReleaseTest $label (Join-Path $build 'pulse_rename_ops_test.exe') `
            "Rename lifecycle check $mode failed" @($mode)
    }
    $env:PULSE_SELFTEST_NO_SCREENSHOTS = '1'
    foreach ($mode in @('--parent-cycle-only', '--quiet-maintenance-only', '--name-pool-only',
        '--maintenance-only', '--usn-only', '--feed-only', '--folder-sizes-only', '--hierarchy-only', '--visibility-cache-only')) {
        $label = 'index' + ($mode -replace '^--', '-')
        Invoke-ReleaseTest $label (Join-Path $build 'pulse_index_engine_test.exe') `
            "Index regression $mode failed" @($mode)
    }
    Invoke-ReleaseTest 'controllers-layout-search-prefs' (Join-Path $build 'pulse_app_controllers_test.exe') `
        'Panel layout/preferences regression failed' @('--layout-search-prefs')
    foreach ($mode in @('--layout-active-pane', '--network-locations-view')) {
        $label = 'controllers' + ($mode -replace '^--', '-')
        Invoke-ReleaseTest $label (Join-Path $build 'pulse_app_controllers_test.exe') `
            "Panel/network navigation regression $mode failed" @($mode)
    }
    Invoke-ReleaseTest 'change-feed-memory' (Join-Path $build 'pulse_change_feed_memory_test.exe') `
        'Opt-in memory probe regression failed' @('--probe')
    foreach ($mode in @('--service-start-only', '--shutdown-only', '--live-dedup-only', '--folder-sizes-only')) {
        $label = 'index-host' + ($mode -replace '^--', '-')
        Invoke-ReleaseTest $label (Join-Path $build 'pulse_index_host_stress.exe') `
            "Index lifecycle check $mode failed" @($mode)
    }
    $selftestCases = @('rename-editor', 'rename-editor-native', 'operation-toast',
        'filter-controls', 'rename-outside', 'address-editor', 'address-editor-native', 'release-panels-hidden', 'pr-shell', 'pin-reorder', 'snapshot-patch', 'list-columns',
        'filename-render', 'filename-render-native', 'folder-views', 'pane-header-icons', 'folder-sizes', 'tray-stack')
    $selftestLogs = @{
        'folder-sizes' = 'bench_data/folder-sizes/results.log'
        'pane-header-icons' = 'bench_data/pane-header-icons/results.log'
        'filename-render' = 'bench_data/filename-render/results.log'
        'filename-render-native' = 'bench_data/filename-render/results.log'
        'rename-editor' = 'bench_data/rename-editor/results.log'
        'rename-editor-native' = 'bench_data/rename-editor/results.log'
        'operation-toast' = 'bench_data/operation-toast/results.log'
        'address-editor' = 'bench_data/address-editor/results.log'
        'address-editor-native' = 'bench_data/address-editor/results.log'
    }
    foreach ($testCase in $selftestCases) {
        $env:PULSE_SELFTEST_CASE = $testCase -replace '-native$', ''
        $env:PULSE_LUMATEXT = if ($testCase.EndsWith('-native')) { '0' } else { '1' }
        $selftest = Start-Process -FilePath (Join-Path $build 'pulse.exe') -ArgumentList '--selftest' -WindowStyle Hidden -PassThru
        $finished = $selftest.WaitForExit(120000)
        if (-not $finished) { $selftest.Kill(); $selftest.WaitForExit() }
        $selftest.Refresh()
        $caseLog = Join-Path $build "selftest-$testCase.log"
        $sourceLog = if ($selftestLogs.ContainsKey($testCase)) { $selftestLogs[$testCase] } else { 'bench_data/selftest_1b2_last.log' }
        Copy-Item $sourceLog $caseLog -ErrorAction SilentlyContinue
        if (-not $finished) { Record-ReleaseTestFailure "Selftest $testCase timed out"; continue }
        if ($selftest.ExitCode -ne 0) {
            $caseFailures = Get-Content $caseLog -ErrorAction SilentlyContinue | Select-String '\[FAIL\]'
            $caseFailures
            $caseFailures | Select-Object -First 40 |
                ForEach-Object { Write-Output "::error::$($_.Line.Trim())" }
            Record-ReleaseTestFailure "Selftest $testCase failed: $($selftest.ExitCode)"
        }
    }
    Remove-Item Env:PULSE_SELFTEST_CASE
    $liveLog = Join-Path $build 'content-live-selection.log'
    $env:PULSE_TEST_SEARCH_FLOW = $liveLog
    $env:PULSE_TEST_CONTENT_LIVE_SELECTION = '1'
    $live = Start-Process -FilePath (Join-Path $build 'pulse.exe') -ArgumentList '--test-instance', '--shot',
        (Join-Path $build 'content-live-selection.png'), $env:TEMP -WindowStyle Hidden -PassThru
    $liveDone = $live.WaitForExit(60000)
    if (-not $liveDone) { $live.Kill(); $live.WaitForExit() }
    $live.Refresh()
    Remove-Item Env:PULSE_TEST_SEARCH_FLOW, Env:PULSE_TEST_CONTENT_LIVE_SELECTION
    if (-not $liveDone -or $live.ExitCode -ne 0) {
        Get-Content $liveLog -ErrorAction SilentlyContinue
        Get-Content $liveLog -ErrorAction SilentlyContinue | Select-String '\[FAIL\]' |
            Select-Object -First 40 | ForEach-Object { Write-Output "::error::$($_.Line.Trim())" }
        Record-ReleaseTestFailure 'Live content selection regression failed'
    }
    Remove-Item Env:PULSE_LUMATEXT
    Write-StageTime 'unit, mode and selftest regressions'
    if ($releaseTestFailures.Count -ne 0) {
        throw ("Release verification failed ({0}): {1}" -f $releaseTestFailures.Count,
            ($releaseTestFailures -join '; '))
    }
    # Strip the embedded test suite and GUI probes from the shipped executable after verification.
    (Get-Content -LiteralPath $batch -Raw).Replace('-DPULSE_WITH_SELFTEST=ON', '-DPULSE_WITH_SELFTEST=OFF -DPULSE_WITH_TEST_HOOKS=OFF').Replace("--target $testTargets", '--target pulse') |
        Set-Content -LiteralPath $batch -Encoding ascii
    & $batch
    if ($LASTEXITCODE -ne 0) { throw 'Production compilation failed' }
    Write-StageTime 'production rebuild'
}
& (Join-Path $repo 'scripts/check_release_payload.ps1') -BuildDir $build
# Content enhancement pack assets of this channel, matching the catalog in pulse.exe.
$contentPack = Join-Path $build 'content-pack'
& (Join-Path $repo 'scripts/check_content_pack.ps1') -PackDir $contentPack -BuildDir $build
$contentDist = Join-Path $repo "dist/content-pack-$Channel"
New-Item -ItemType Directory -Path $contentDist -Force | Out-Null
Copy-Item -Path (Join-Path $contentPack '*') -Destination $contentDist -Force
# AI enhancement pack assets of this channel, likewise.
$aiPack = Join-Path $build 'ai-pack'
& (Join-Path $repo 'scripts/check_content_pack.ps1') -Key ai -PackDir $aiPack -BuildDir $build
$aiDist = Join-Path $repo "dist/ai-pack-$Channel"
New-Item -ItemType Directory -Path $aiDist -Force | Out-Null
Copy-Item -Path (Join-Path $aiPack '*') -Destination $aiDist -Force
if ($Channel -eq 'win81') {
    python tools/audit_win81_imports.py $build
    if ($LASTEXITCODE -ne 0) { throw 'Windows 8.1 startup import guard failed' }
}
$licenses = Join-Path $build 'licenses/LumaText'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -Path (Join-Path $sdkRoot 'share/LumaText/licenses/*') -Destination $licenses
# Pulse setup: bootstrapper + xz payload (scripts/pack_setup.bat).
& (Join-Path $repo 'scripts/pack_setup.bat') $build $Channel
if ($LASTEXITCODE -ne 0) { throw 'Installer packaging failed' }
if (Get-Command sccache -ErrorAction SilentlyContinue) { sccache --show-stats }
Write-StageTime 'payload, packs and installer'
