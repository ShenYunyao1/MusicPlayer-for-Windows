param(
    [string]$BuildDirectory = '',
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
function Get-PackageSHA256([string]$path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $inputStream = [IO.File]::OpenRead($path)
    try {
        return [BitConverter]::ToString($algorithm.ComputeHash($inputStream)).Replace('-','').ToLowerInvariant()
    } finally { $inputStream.Dispose(); $algorithm.Dispose() }
}
try {
    if (!$BuildDirectory) { $BuildDirectory = Join-Path $PSScriptRoot 'build' }
    if (!$OutputDirectory) { $OutputDirectory = Join-Path $PSScriptRoot 'dist' }
    $BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
    $OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
    $files = @{}
    foreach ($name in @('MusicPlayer2.exe','MusicPlayerUpdater.exe','bass.dll','bass_fx.dll','tag.dll','SciLexer.dll')) {
        $path = Join-Path $BuildDirectory $name
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing $name. Build x64 Release first." }
        $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($path))
        try {
            if ($reader.ReadUInt16() -ne 0x5a4d) { throw "Invalid PE file: $name" }
            $null = $reader.BaseStream.Seek(0x3c,[IO.SeekOrigin]::Begin)
            $offset = $reader.ReadInt32()
            $null = $reader.BaseStream.Seek($offset,[IO.SeekOrigin]::Begin)
            if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) {
                throw "Expected x64 PE file: $name"
            }
        } finally { $reader.Dispose() }
        $files[$name] = $path
    }
    function Add-PackageFolder([string]$source, [string]$target) {
        if (!(Test-Path -LiteralPath $source -PathType Container)) { throw "Missing runtime resources: $source" }
        $source = (Resolve-Path -LiteralPath $source).Path
        foreach ($file in Get-ChildItem -LiteralPath $source -File -Recurse) {
            $relative = $file.FullName.Substring($source.Length+1).Replace('\','/')
            $files[$target+'/'+$relative] = $file.FullName
        }
    }
    # Authoritative resources, never the developer's runtime configuration.
    Add-PackageFolder (Join-Path $PSScriptRoot 'MusicPlayer2/language') 'language'
    Add-PackageFolder (Join-Path $PSScriptRoot 'MusicPlayer2/skins') 'skins'
    Add-PackageFolder (Join-Path $PSScriptRoot 'third_party/runtime/common/Encoder') 'Encoder'
    $files['global_cfg.ini'] = Join-Path $PSScriptRoot 'third_party/runtime/defaults/global_cfg.ini'
    $files['LICENSE'] = Join-Path $PSScriptRoot 'LICENSE'
    $files['README.txt'] = Join-Path $PSScriptRoot 'Documents/portable-readme.txt'
    foreach ($file in $files.Values) {
        if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing package input: $file" }
    }

    if (!('ClientPackageResources' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ClientPackageResources {
    [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
    [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr FindResourceW(IntPtr module, IntPtr name, string type);
    [DllImport("kernel32", SetLastError=true)] public static extern uint SizeofResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32", SetLastError=true)] public static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32")] public static extern IntPtr LockResource(IntPtr resource);
    [DllImport("kernel32")] public static extern bool FreeLibrary(IntPtr module);
}
'@
    }
    $module = [ClientPackageResources]::LoadLibraryExW($files['MusicPlayer2.exe'],[IntPtr]::Zero,2)
    if ($module -eq [IntPtr]::Zero) { throw 'Unable to inspect executable build information.' }
    try {
        $resource = [ClientPackageResources]::FindResourceW($module,[IntPtr]540,'TEXT')
        if ($resource -eq [IntPtr]::Zero) { throw 'Executable has no embedded Git build information.' }
        $length = [ClientPackageResources]::SizeofResource($module,$resource)
        $memory = [ClientPackageResources]::LockResource([ClientPackageResources]::LoadResource($module,$resource))
        $bytes = New-Object byte[] $length
        [Runtime.InteropServices.Marshal]::Copy($memory,$bytes,0,[int]$length)
        $stamp = [Text.Encoding]::UTF8.GetString($bytes).Trim()
    } finally { $null = [ClientPackageResources]::FreeLibrary($module) }
    $stampLines = $stamp -split '\r?\n'
    if ($stampLines.Length -lt 2 -or $stampLines[1].Trim() -notmatch '^[a-f0-9]{40}$') {
        throw 'Executable build information is incomplete. Rebuild before packaging.'
    }
    $exeHash = Get-PackageSHA256 $files['MusicPlayer2.exe']
    $release = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'client-release.json') -Raw | ConvertFrom-Json
    if ($release.version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') { throw 'Invalid client release version' }
    $info = "Release: $($release.version)`nPlatform: windows-x64`nBuilt: $($stampLines[0].Trim())`nGit: $($stampLines[1].Trim())`nMusicPlayer2.exe SHA-256: $exeHash`n"
    $inventory = [ordered]@{}
    foreach ($name in ($files.Keys | Sort-Object)) {
        if ($name -cmatch '^(language|skins|Encoder)/') { $inventory[$name] = Get-PackageSHA256 $files[$name] }
    }

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $null = New-Item -ItemType Directory -Path $OutputDirectory -Force
    $archivePath = Join-Path $OutputDirectory 'MusicPlayer2.zip'
    $temporary = $archivePath+'.partial'
    $stream = [IO.File]::Open($temporary,[IO.FileMode]::Create)
    $archive = [IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in ($files.Keys | Sort-Object)) {
            $null = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$files[$name],
                'MusicPlayer2/'+$name,[IO.Compression.CompressionLevel]::Optimal)
        }
        $entry = $archive.CreateEntry('MusicPlayer2/BUILD-INFO.txt')
        $writer = [IO.StreamWriter]::new($entry.Open(),[Text.UTF8Encoding]::new($false))
        try { $writer.Write($info) } finally { $writer.Dispose() }
        $entry = $archive.CreateEntry('MusicPlayer2/PROGRAM-FILES.json')
        $writer = [IO.StreamWriter]::new($entry.Open(),[Text.UTF8Encoding]::new($false))
        try { $writer.Write(($inventory | ConvertTo-Json -Depth 3)) } finally { $writer.Dispose() }
    } finally { $archive.Dispose(); $stream.Dispose() }
    Move-Item -LiteralPath $temporary -Destination $archivePath -Force
    $hash = Get-PackageSHA256 $archivePath
    [IO.File]::WriteAllText($archivePath+'.sha256',"$hash  MusicPlayer2.zip`n",[Text.UTF8Encoding]::new($false))
    Write-Host 'PACKAGE SUCCEEDED' -ForegroundColor Green
    Write-Host "Archive: $archivePath"
    Write-Host "SHA-256: $hash"
    Write-Host "Git: $($stampLines[1].Trim())"
    Write-Host 'Distribute this ZIP, not the build directory.'
    exit 0
} catch {
    Write-Host "PACKAGE FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
