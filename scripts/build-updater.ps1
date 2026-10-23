param([string]$Go = 'go')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$config = Get-Content (Join-Path $root 'client-release.json') -Raw | ConvertFrom-Json
$commit = & git -C $root rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Unable to determine client commit' }
Push-Location (Join-Path $root 'updater')
try {
    $env:CGO_ENABLED = '0'
    & $Go build -trimpath -ldflags "-H=windowsgui -X main.currentVersion=$($config.version) -X main.currentCommit=$commit" -o (Join-Path $root 'build/MusicPlayerUpdater.exe') ./cmd/musicplayer-updater
    if ($LASTEXITCODE -ne 0) { throw 'Updater build failed' }
    Write-Host 'UPDATER BUILD SUCCEEDED' -ForegroundColor Green
} finally { Pop-Location }
