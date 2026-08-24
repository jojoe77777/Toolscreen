param(
    [Parameter(Mandatory = $true)]
    [string]$ArtifactDirectory
)

$ErrorActionPreference = 'Stop'

function Get-CMakeVersionString {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(Mandatory = $true)]
        [string]$Prefix
    )

    if (-not (Test-Path -LiteralPath $FilePath)) {
        throw "Missing version file '$FilePath'."
    }

    $content = Get-Content -LiteralPath $FilePath -Raw
    $majorMatch = [regex]::Match($content, "set\($Prefix`_VERSION_MAJOR\s+(\d+)\)")
    $minorMatch = [regex]::Match($content, "set\($Prefix`_VERSION_MINOR\s+(\d+)\)")
    $patchMatch = [regex]::Match($content, "set\($Prefix`_VERSION_PATCH\s+(\d+)\)")

    if (-not $majorMatch.Success -or -not $minorMatch.Success -or -not $patchMatch.Success) {
        throw "Failed to parse $Prefix version from '$FilePath'."
    }

    return '{0}.{1}.{2}' -f $majorMatch.Groups[1].Value, $minorMatch.Groups[1].Value, $patchMatch.Groups[1].Value
}

function Get-NormalizedVersionString {
    param(
        [string]$Value
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $null
    }

    $parts = [regex]::Matches($Value, '\d+') | ForEach-Object { $_.Value }
    if ($parts.Count -eq 0) {
        return $null
    }

    while ($parts.Count -gt 3 -and $parts[-1] -eq '0') {
        $parts = $parts[0..($parts.Count - 2)]
    }

    return ($parts -join '.')
}

function Get-SingleArtifact {
    param(
        [string]$Root,
        [string]$Pattern,
        [string]$Label
    )

    $artifactMatches = Get-ChildItem -Path $Root -File -Filter $Pattern -ErrorAction Stop
    if ($artifactMatches.Count -ne 1) {
        throw "Expected exactly one $Label matching '$Pattern' in '$Root', found $($artifactMatches.Count)."
    }

    return $artifactMatches[0]
}

function Assert-Metadata {
    param(
        [System.IO.FileInfo]$File,
        [string]$ExpectedProductName,
        [string]$ExpectedDescription,
        [string]$ExpectedOriginalFilename,
        [string]$ExpectedVersion
    )

    $info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($File.FullName)
    $normalizedFileVersion = Get-NormalizedVersionString $info.FileVersion
    $normalizedProductVersion = Get-NormalizedVersionString $info.ProductVersion

    if ($info.ProductName -ne $ExpectedProductName) {
        throw "Unexpected ProductName for '$($File.Name)': '$($info.ProductName)'"
    }

    if ($info.CompanyName -ne 'jojoe77777') {
        throw "Unexpected CompanyName for '$($File.Name)': '$($info.CompanyName)'"
    }

    if ($info.FileDescription -ne $ExpectedDescription) {
        throw "Unexpected FileDescription for '$($File.Name)': '$($info.FileDescription)'"
    }

    if ($info.OriginalFilename -ne $ExpectedOriginalFilename) {
        throw "Unexpected OriginalFilename for '$($File.Name)': '$($info.OriginalFilename)'"
    }

    if ($info.LegalCopyright -ne 'Copyright (c) 2026 jojoe77777') {
        throw "Unexpected LegalCopyright for '$($File.Name)': '$($info.LegalCopyright)'"
    }

    if (-not $normalizedFileVersion) {
        throw "Missing FileVersion for '$($File.Name)'"
    }

    if (-not $normalizedProductVersion) {
        throw "Missing ProductVersion for '$($File.Name)'"
    }

    if ($normalizedFileVersion -ne $normalizedProductVersion) {
        throw "FileVersion/ProductVersion mismatch for '$($File.Name)': '$normalizedFileVersion' vs '$normalizedProductVersion'"
    }

    if ($normalizedProductVersion -ne $ExpectedVersion) {
        throw "Unexpected ProductVersion for '$($File.Name)': '$normalizedProductVersion' (expected '$ExpectedVersion')"
    }
}

$repoRoot = Split-Path -Path $PSScriptRoot -Parent
$expectedToolscreenVersion = Get-CMakeVersionString -FilePath (Join-Path $repoRoot 'ToolscreenVersion.cmake') -Prefix 'TOOLSCREEN'
$expectedLibloggerVersion = Get-CMakeVersionString -FilePath (Join-Path $repoRoot 'liblogger\LibLoggerVersion.cmake') -Prefix 'LIBLOGGER'

$artifactRoot = (Resolve-Path $ArtifactDirectory).Path
$dll = Get-SingleArtifact -Root $artifactRoot -Pattern 'Toolscreen.dll' -Label 'Toolscreen DLL'
$loggerDll = Get-SingleArtifact -Root $artifactRoot -Pattern 'liblogger_*.dll' -Label 'liblogger DLL'

Assert-Metadata -File $dll -ExpectedProductName 'Toolscreen' -ExpectedDescription 'Toolscreen hook DLL' -ExpectedOriginalFilename 'Toolscreen.dll' -ExpectedVersion $expectedToolscreenVersion
Assert-Metadata -File $loggerDll -ExpectedProductName 'liblogger' -ExpectedDescription 'LibLogger' -ExpectedOriginalFilename $loggerDll.Name -ExpectedVersion $expectedLibloggerVersion

Write-Host "Verified unsigned PE metadata for Toolscreen artifacts in '$artifactRoot'."
