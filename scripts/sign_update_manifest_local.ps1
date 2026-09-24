#requires -Version 5.1
<#
Signs an update manifest the way scripts/create_update_manifest.ps1 does, without requiring
PowerShell 7.2: that script uses .NET 5+ APIs (ImportFromPem / DSASignatureFormat) that Windows
PowerShell 5.1 does not have. This one signs with the bundled Git openssl and converts its DER
signature into the raw r||s (IEEE-P1363) form the client verifies, then verifies the result
before writing the manifest.

The payload is byte-for-byte the same as the official script produces, so a manifest written here
is indistinguishable from one written by CI - same fields, same signature format, same key.

Usage:
    powershell -File scripts/sign_update_manifest_local.ps1 `
        -Installer dist/PulseSetup-1.0.37-dev.exe `
        -DownloadPage https://github.com/4B-Rubber/pulse/releases/download/v1.0.37/PulseSetup-1.0.37-dev.exe
#>
param(
    [Parameter(Mandatory = $true)][string]$Installer,
    [Parameter(Mandatory = $true)][string]$DownloadPage,
    [string]$PrivateKey = "$env:USERPROFILE\.pulse-dev-channel\update-private-key.pem",
    [uint32]$MinimumWindowsBuild = 19045,
    [string]$Output = "",
    [string]$OpenSsl = "C:\Program Files\Git\usr\bin\openssl.exe"
)

# openssl reports progress ("read EC key\n") on stderr; Windows PowerShell 5.1 turns native stderr
# into an error record, and with 'Stop' that aborts the script. Every native call is followed by an
# explicit exit-code check instead.
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path -LiteralPath $OpenSsl)) { throw "openssl not found at $OpenSsl (pass -OpenSsl)" }
$version = ([IO.File]::ReadAllText((Join-Path $repo 'version.txt'))).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'version.txt must contain a three-part numeric version' }
if ($DownloadPage -notmatch '^https://') { throw 'DownloadPage must use HTTPS' }
if ($MinimumWindowsBuild -lt 9600) { throw 'MinimumWindowsBuild must identify Windows 8.1 (9600) or newer' }

$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$privateKeyPath = (Resolve-Path -LiteralPath $PrivateKey).Path
if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path (Split-Path -Parent $installerPath) 'update-manifest.json'
}
$hash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToLowerInvariant()
$payload = "schema=1`nversion=$version`nminimum_windows_build=$MinimumWindowsBuild`n" +
           "download_page=$DownloadPage`ninstaller_sha256=$hash`n"

$payloadPath = Join-Path ([IO.Path]::GetTempPath()) ('pulse-payload-' + [guid]::NewGuid().ToString('N') + '.bin')
$derPath = Join-Path ([IO.Path]::GetTempPath()) ('pulse-sig-' + [guid]::NewGuid().ToString('N') + '.der')
$pubPath = Join-Path ([IO.Path]::GetTempPath()) ('pulse-pub-' + [guid]::NewGuid().ToString('N') + '.pem')
try {
    [IO.File]::WriteAllBytes($payloadPath, [Text.Encoding]::UTF8.GetBytes($payload))

    # Sign: openssl emits a DER SEQUENCE { INTEGER r, INTEGER s }.
    & $OpenSsl dgst -sha256 -sign $privateKeyPath -out $derPath $payloadPath 2>$null
    if ($LASTEXITCODE -ne 0) { throw 'openssl signing failed' }
    $der = [IO.File]::ReadAllBytes($derPath)

    function Read-DerInteger([byte[]]$data, [ref]$index) {
        if ($data[$index.Value] -ne 0x02) { throw 'unexpected DER structure' }
        $length = $data[$index.Value + 1]
        $start = $index.Value + 2
        $bytes = $data[$start..($start + $length - 1)]
        $index.Value = $start + $length
        $trimmed = $bytes
        while ($trimmed.Length -gt 1 -and $trimmed[0] -eq 0) { $trimmed = $trimmed[1..($trimmed.Length - 1)] }
        return , $trimmed
    }

    $index = 0
    if ($der[0] -ne 0x30) { throw 'unexpected DER header' }
    $index = 2
    if ($der[1] -band 0x80) { $index = 2 + ($der[1] -band 0x7F) }
    $r = Read-DerInteger $der ([ref]$index)
    $s = Read-DerInteger $der ([ref]$index)
    if ($r.Length -gt 32 -or $s.Length -gt 32) { throw 'signature integer wider than the P-256 field' }

    $raw = New-Object byte[] 64
    [Array]::Copy($r, 0, $raw, 32 - $r.Length, $r.Length)
    [Array]::Copy($s, 0, $raw, 64 - $s.Length, $s.Length)

    # Self-check: turn the raw signature back into DER and let openssl verify it, so a mistyped
    # conversion cannot end up in a published manifest.
    $rPad = New-Object byte[] 32
    [Array]::Copy($raw, 0, $rPad, 0, 32)
    $sPad = New-Object byte[] 32
    [Array]::Copy($raw, 32, $sPad, 0, 32)

    function New-DerInteger([byte[]]$value) {
        $v = $value
        while ($v.Length -gt 1 -and $v[0] -eq 0) { $v = $v[1..($v.Length - 1)] }
        if ($v[0] -band 0x80) { $v = (@([byte]0) + @($v)) }
        return (@(0x02, [byte]$v.Length) + @($v))
    }

    $body = @(New-DerInteger $rPad) + @(New-DerInteger $sPad)
    $verifyDer = [byte[]](@(0x30, [byte]$body.Length) + $body)
    # The rebuilt DER must be byte-identical to what openssl produced: that is what proves the raw
    # r||s in the manifest is the signature of this payload and not a shifted or padded variant.
    if ([Convert]::ToBase64String($verifyDer) -ne [Convert]::ToBase64String($der)) {
        throw 'raw r||s does not re-encode to the signature openssl produced'
    }
    $derPath2 = $derPath + '.verify'
    [IO.File]::WriteAllBytes($derPath2, $verifyDer)
    & $OpenSsl ec -in $privateKeyPath -pubout -out $pubPath 2>$null
    $verdict = (& $OpenSsl dgst -sha256 -verify $pubPath -signature $derPath2 $payloadPath 2>&1) -join ' '
    Remove-Item -LiteralPath $derPath2 -Force -ErrorAction SilentlyContinue
    if ($verdict -notmatch 'Verified OK') { throw "round-trip verification failed: $verdict" }

    $signature = [Convert]::ToBase64String($raw)
    $manifest = [ordered]@{
        schema                = 1
        version               = $version
        minimum_windows_build = $MinimumWindowsBuild
        download_page         = $DownloadPage
        installer_sha256      = $hash
        signature             = $signature
    } | ConvertTo-Json
    [IO.File]::WriteAllText($Output, $manifest + "`n", [Text.UTF8Encoding]::new($false))

    Write-Host "Update manifest written to $Output"
    Write-Host "  version            : $version"
    Write-Host "  minimum win build  : $MinimumWindowsBuild"
    Write-Host "  installer          : $installerPath"
    Write-Host "  installer_sha256   : $hash"
    Write-Host "  signature bytes    : $($raw.Length) (raw r||s, verified round-trip)"
} finally {
    Remove-Item -LiteralPath $payloadPath, $derPath, $pubPath -Force -ErrorAction SilentlyContinue
}
