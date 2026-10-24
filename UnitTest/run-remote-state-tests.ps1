$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$install = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$install) { throw 'Visual Studio C++ tools required' }
$vcvars = Join-Path $install 'VC/Auxiliary/Build/vcvars64.bat'
$lines = & $env:ComSpec /d /c "`"$vcvars`" >nul && set"
foreach ($line in $lines) { $i=$line.IndexOf('='); if ($i -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0,$i),$line.Substring($i+1),'Process') } }
$output = Join-Path $PSScriptRoot '../test-results/remote-state'
$null = New-Item -ItemType Directory -Path $output -Force
Push-Location $output
try {
    & cl.exe /nologo /std:c++17 /EHsc /utf-8 (Join-Path $PSScriptRoot 'remote_library_state.cpp') /Fe:remote_library_state.exe
    if ($LASTEXITCODE -ne 0) { throw 'Remote state test build failed' }
    ./remote_library_state.exe
    if ($LASTEXITCODE -ne 0) { throw 'Remote state regression failed' }
} finally { Pop-Location }
