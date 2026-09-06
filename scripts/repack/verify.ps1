param(
    [Parameter(Mandatory)][string]$ArtifactDirectory,
    [ValidatePattern('^\d+\.\d+\.\d+$')][string]$InstallerVersion = '1.4.5',
    [switch]$RequireSigned
)
. "$PSScriptRoot/common.ps1"
$root = (Resolve-Path -LiteralPath $ArtifactDirectory).Path
$baseName = "Toolscreen-$InstallerVersion-double-click-me"
$jarPath = Join-Path $root "$baseName.jar"
$exePath = Join-Path $root "$baseName.exe"
$inspection = Join-Path ([IO.Path]::GetTempPath()) ('toolscreen-payload-check-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $inspection | Out-Null

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($jarPath)
try {
    $actual = @($archive.Entries | Where-Object { $_.FullName.StartsWith('dlls/') -and $_.Name } | ForEach-Object FullName | Sort-Object)
    $expected = @($RepackManifest.payloads | ForEach-Object { "dlls/$($_.packagedName)" } | Sort-Object)
    if (Compare-Object $expected $actual) { throw 'Unexpected DLL/support payloads in JAR' }
    foreach ($payload in $RepackManifest.payloads) {
        $entry = $archive.GetEntry("dlls/$($payload.packagedName)")
        $destination = Join-Path $inspection "jar-$($payload.packagedName)"
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination)
        Assert-RepackPayload $destination $payload
    }
    $reader = [IO.StreamReader]::new($archive.GetEntry('branding.properties').Open())
    try { $branding = $reader.ReadToEnd() } finally { $reader.Dispose() }
    if ($branding -notmatch "(?m)^brand.version=$([regex]::Escape($InstallerVersion))\r?$") {
        throw 'Wrong JAR branding version'
    }
} finally { $archive.Dispose() }

if (-not ('ToolscreenRepackResources' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ToolscreenRepackResources {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr LoadLibraryEx(string path, IntPtr reserved, uint flags);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern IntPtr FindResource(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32.dll")] static extern uint SizeofResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll")] static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll")] static extern IntPtr LockResource(IntPtr resource);
    [DllImport("kernel32.dll")] public static extern bool FreeLibrary(IntPtr module);
    public static byte[] Read(IntPtr module, int id) {
        var resource = FindResource(module, (IntPtr)id, (IntPtr)10);
        if (resource == IntPtr.Zero) throw new Exception("Missing RCDATA resource " + id);
        var size = checked((int)SizeofResource(module, resource));
        var data = LockResource(LoadResource(module, resource));
        if (data == IntPtr.Zero) throw new Exception("Unreadable resource " + id);
        var bytes = new byte[size]; Marshal.Copy(data, bytes, 0, size); return bytes;
    }
}
'@
}
# LOAD_LIBRARY_AS_DATAFILE: inspect resource bytes without executing the installer.
$module = [ToolscreenRepackResources]::LoadLibraryEx($exePath, [IntPtr]::Zero, 2)
if ($module -eq [IntPtr]::Zero) { throw "Cannot inspect EXE: $exePath" }
try {
    $index = [Text.Encoding]::UTF8.GetString([ToolscreenRepackResources]::Read($module, 102))
    $entries = @($index.Trim().Split("`n") | ForEach-Object {
        $parts = $_.Trim().Split('|')
        if ($parts.Count -ne 2) { throw 'Invalid EXE payload index' }
        [pscustomobject]@{ Id = [int]$parts[0]; Name = $parts[1] }
    })
    if (Compare-Object @($RepackManifest.payloads.packagedName | Sort-Object) @($entries.Name | Sort-Object)) {
        throw 'Unexpected EXE payloads'
    }
    foreach ($payload in $RepackManifest.payloads) {
        $entry = $entries | Where-Object Name -CEQ $payload.packagedName
        $destination = Join-Path $inspection "exe-$($payload.packagedName)"
        [IO.File]::WriteAllBytes($destination, [ToolscreenRepackResources]::Read($module, $entry.Id))
        Assert-RepackPayload $destination $payload
    }
} finally { [void][ToolscreenRepackResources]::FreeLibrary($module) }

$info = [Diagnostics.FileVersionInfo]::GetVersionInfo($exePath)
if ($info.FileVersion -ne $InstallerVersion -or $info.ProductVersion -ne $InstallerVersion -or
    $info.ProductName -ne 'Toolscreen' -or $info.FileDescription -ne 'Toolscreen installer' -or
    $info.CompanyName -ne 'jojoe77777' -or $info.OriginalFilename -ne "$baseName.exe") {
    throw 'Unexpected installer PE metadata'
}
if ($RequireSigned) {
    Assert-RepackSignature $exePath
    $verification = & jarsigner '-J-Duser.language=en' '-J-Duser.country=US' -verify $jarPath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or $verification -notmatch 'jar verified\.' -or $verification -match 'contains unsigned entries') {
        throw "JAR signature verification failed: $verification"
    }
}
Write-Host "Verified installer $InstallerVersion and byte-identical 1.4.4 payloads (signed=$RequireSigned)."
