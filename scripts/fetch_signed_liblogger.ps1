[CmdletBinding()]
param(
    [string]$DestinationDirectory = (Join-Path (Join-Path $PSScriptRoot '..') 'out\prebuilt-liblogger'),
    [string]$Owner = 'jojoe77777',
    [string]$Repository = 'Toolscreen',
    [string]$ReleaseTag = 'liblogger-signed-latest',
    [ValidateSet('x64', 'arm64', 'all')]
    [string]$Architecture = $(if ($env:TOOLSCREEN_LIBLOGGER_ARCH) { $env:TOOLSCREEN_LIBLOGGER_ARCH } else { 'x64' })
)

$ErrorActionPreference = 'Stop'

$architectures = if ($Architecture -eq 'all') { @('x64', 'arm64') } else { @($Architecture) }
$assetNames = foreach ($arch in $architectures) {
    "liblogger_$arch.dll"
    "liblogger_$arch.pdb"
}

$headers = @{
    'User-Agent' = 'Toolscreen-build'
    'X-GitHub-Api-Version' = '2022-11-28'
}

$token = $env:GITHUB_TOKEN
if (-not $token) {
    $token = $env:GH_TOKEN
}

if ($token) {
    $headers['Authorization'] = "Bearer $token"
}

function Remove-IfExists {
    param([string]$Path)

    if (Test-Path $Path) {
        Remove-Item -Path $Path -Force
    }
}

function Download-AssetsDirect {
    param(
        [string]$BaseUrl,
        [string]$TargetDirectory,
        [string[]]$Names,
        [hashtable]$RequestHeaders
    )

    foreach ($assetName in $Names) {
        $destinationPath = Join-Path $TargetDirectory $assetName
        Remove-IfExists -Path $destinationPath
        try {
            Invoke-WebRequest -Uri "$BaseUrl/$assetName" -Headers $RequestHeaders -OutFile $destinationPath
        } catch {
            Remove-IfExists -Path $destinationPath
            throw
        }
    }
}

$releaseDownloadBaseUrl = "https://github.com/$Owner/$Repository/releases/download/$ReleaseTag"
$releaseUrl = "https://api.github.com/repos/$Owner/$Repository/releases/tags/$ReleaseTag"

New-Item -ItemType Directory -Force -Path $DestinationDirectory | Out-Null

try {
    Download-AssetsDirect -BaseUrl $releaseDownloadBaseUrl -TargetDirectory $DestinationDirectory -Names $assetNames -RequestHeaders $headers
} catch {
    try {
        $release = Invoke-RestMethod -Uri $releaseUrl -Headers $headers
    } catch {
        throw "Failed to download liblogger assets from '$releaseDownloadBaseUrl' or resolve the '$ReleaseTag' release for $Owner/$Repository. Run the manual 'Build Liblogger' workflow first. $($_.Exception.Message)"
    }

    foreach ($assetName in $assetNames) {
        $asset = $release.assets | Where-Object { $_.name -eq $assetName } | Select-Object -First 1

        if (-not $asset) {
            throw "Release '$ReleaseTag' is missing required asset '$assetName'."
        }

        $destinationPath = Join-Path $DestinationDirectory $assetName
        Remove-IfExists -Path $destinationPath
        Invoke-WebRequest -Uri $asset.browser_download_url -Headers $headers -OutFile $destinationPath
    }
}

Write-Host "Downloaded signed liblogger assets to $DestinationDirectory"
