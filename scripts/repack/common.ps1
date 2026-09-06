$ErrorActionPreference = 'Stop'
$RepackManifest = Get-Content (Join-Path $PSScriptRoot 'payload.json') -Raw | ConvertFrom-Json

function Assert-RepackHash([string]$Path, [string]$Expected) {
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $Expected) {
        throw "SHA-256 mismatch: $Path"
    }
}

function Assert-RepackSignature([string]$Path) {
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'CN=SignPath Foundation(?:,|$)') {
        throw "Expected a valid SignPath Foundation signature: $Path ($($signature.Status))"
    }
}

function Assert-RepackPayload([string]$Path, $Payload) {
    Assert-RepackHash $Path $Payload.sha256
    Assert-RepackSignature $Path
    $bytes = [IO.File]::ReadAllBytes($Path)
    $peOffset = [BitConverter]::ToInt32($bytes, 60)
    if ([BitConverter]::ToUInt16($bytes, $peOffset + 4) -ne 0x8664) {
        throw "Expected an x64 PE payload: $Path"
    }
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($Path).FileVersion
    if ($version -notin @($Payload.version, "$($Payload.version).0")) {
        throw "Unexpected payload version '$version': $Path"
    }
}

function Assert-NativeExit([string]$Operation) {
    if ($LASTEXITCODE -ne 0) { throw "$Operation failed with exit code $LASTEXITCODE" }
}
