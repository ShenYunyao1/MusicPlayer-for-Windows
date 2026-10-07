param(
    [string]$Python = 'python',
    [string]$MfcRoot = '',
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere is required.' }
$installations = @(& $vswhere -all -products '*' -format json | ConvertFrom-Json)
$build = $installations | Where-Object { Test-Path (Join-Path $_.installationPath 'VC\Auxiliary\Build\vcvars64.bat') } | Select-Object -First 1
if (-not $build) { throw 'Visual Studio C++ x64 build tools are required.' }
if (-not $MfcRoot) {
    foreach ($installation in $installations) {
        $toolsets = Get-ChildItem -LiteralPath (Join-Path $installation.installationPath 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
        foreach ($toolset in $toolsets) {
            $candidate = Join-Path $toolset.FullName 'atlmfc'
            if (Test-Path (Join-Path $candidate 'lib\x64\mfcs140.lib')) { $MfcRoot = $candidate; break }
        }
        if ($MfcRoot) { break }
    }
}
if (-not $MfcRoot) { throw 'Visual Studio MFC x64 components are required.' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path ([IO.Path]::GetTempPath()) ('mp2-cache-tests-' + [guid]::NewGuid()) }
$null = New-Item -ItemType Directory -Path $OutputDirectory -Force
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$vcvars = Join-Path $build.installationPath 'VC\Auxiliary\Build\vcvars64.bat'
$environment = & $env:ComSpec /d /c "`"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the compiler environment.' }
foreach ($line in $environment) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0, $separator), $line.Substring($separator + 1), 'Process') }
}
$project = Join-Path $PSScriptRoot '..\MusicPlayer2'
$exe = Join-Path $OutputDirectory 'remote_library_cache.exe'
Push-Location $OutputDirectory
try {
    & cl.exe /nologo /std:c++17 /EHsc /MD /utf-8 /D_AFXDLL /D_UNICODE /DUNICODE "/I$MfcRoot\include" "/I$project" (Join-Path $PSScriptRoot 'remote_library_cache.cpp') (Join-Path $project 'RemoteLibraryClient.cpp') "/Fe:$exe" /link "/LIBPATH:$MfcRoot\lib\x64" "/LIBPATH:$project"
    if ($LASTEXITCODE -ne 0) { throw 'Remote cache test build failed.' }
    & $Python (Join-Path $PSScriptRoot 'remote_library_fixture.py') --exe $exe
    if ($LASTEXITCODE -ne 0) { throw 'Remote cache regression failed.' }
} finally { Pop-Location }
