param(
    [Parameter(Mandatory)][string]$SourceDirectory,
    [Parameter(Mandatory)][string]$DownloadDirectory,
    [ValidatePattern('^\d+\.\d+\.\d+$')][string]$InstallerVersion = '1.4.5'
)
. "$PSScriptRoot/common.ps1"
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
$commit = git -C $source rev-parse HEAD
Assert-NativeExit 'Resolve EasyInject commit'
if ($commit -ne $RepackManifest.easyInjectCommit) { throw "Unexpected EasyInject commit: $commit" }
git -C $source diff --exit-code --quiet HEAD
Assert-NativeExit 'Require an unmodified EasyInject checkout'

$patch = Join-Path $PSScriptRoot 'easyinject-x64.patch'
git -C $source apply --check $patch
Assert-NativeExit 'Check x64 packaging patch'
git -C $source apply $patch
Assert-NativeExit 'Apply x64 packaging patch'

New-Item -ItemType Directory -Force -Path $DownloadDirectory | Out-Null
$archivePath = Join-Path (Resolve-Path $DownloadDirectory).Path 'Toolscreen-1.4.4-double-click-me.jar'
if (-not (Test-Path -LiteralPath $archivePath)) {
    Invoke-WebRequest -Uri $RepackManifest.sourceUrl -OutFile $archivePath
}
Assert-RepackHash $archivePath $RepackManifest.sourceSha256

# These are dedicated packaging input directories in the freshly checked-out dependency.
# Remove individual input files so tracked/stale payloads cannot enter the package.
foreach ($relative in @('custom-dlls', 'src/main/resources/dlls')) {
    $directory = Join-Path $source $relative
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    Get-ChildItem -LiteralPath $directory -File | ForEach-Object {
        Remove-Item -LiteralPath $_.FullName -Force
    }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    foreach ($payload in $RepackManifest.payloads) {
        $entries = @($archive.Entries | Where-Object FullName -CEQ $payload.entry)
        if ($entries.Count -ne 1) { throw "Expected exactly one $($payload.entry)" }
        $destination = Join-Path $source $payload.destination
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entries[0], $destination, $true)
        Assert-RepackPayload $destination $payload
    }
} finally { $archive.Dispose() }

$brandingPath = Join-Path $source 'branding.properties'
$branding = Get-Content -LiteralPath $brandingPath -Raw
if ($branding -notmatch '(?m)^brand.version=') { throw 'Missing brand.version' }
$branding = [regex]::Replace($branding, '(?m)^brand.version=[^\r\n]*', "brand.version=$InstallerVersion")
[IO.File]::WriteAllText($brandingPath, $branding, [Text.UTF8Encoding]::new($false))
Write-Host "Prepared installer $InstallerVersion with the exact signed x64 1.4.4 payload."
