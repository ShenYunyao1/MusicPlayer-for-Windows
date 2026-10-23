param(
    [ValidateSet('Build', 'Release')][string]$Mode,
    [string]$Version = '',
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')][string]$Remote = 'origin',
    [string]$Proxy = '',
    [ValidateRange(1, 180)][int]$TimeoutMinutes = 60,
    [ValidateRange(1, 60)][int]$PollSeconds = 15,
    [switch]$DryRun,
    [switch]$NoWait,
    [switch]$Help,
    [string]$GitPath = 'git.exe',
    [string]$GhPath = 'gh.exe'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($Help -or !$Mode) {
    Write-Host 'Usage: publish.bat -Mode Build [-DryRun] [-NoWait] [-Proxy URL]'
    Write-Host '       publish.bat -Mode Release [-Version X.Y.Z] [-DryRun] [-NoWait]'
    Write-Host 'Commit version and release notes first. No arguments only displays help.'
    exit 0
}
$root = $PSScriptRoot
$workflow = '.github/workflows/windows-release.yml'
$savedEnvironment = @{}
foreach ($name in @('GH_TOKEN', 'GH_CONFIG_DIR', 'GH_HOST', 'HTTPS_PROXY')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$localTagCreated = $false
$pushed = $false
$tag = ''

function Invoke-Tool([string]$Executable, [string[]]$Arguments, [switch]$AllowFailure, [string]$InputText = '') {
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $PSNativeCommandUseErrorActionPreference = $false
    try {
        $global:LASTEXITCODE = 0
        if ($InputText) { $lines = @($InputText | & $Executable @Arguments 2>&1) }
        else { $lines = @(& $Executable @Arguments 2>&1) }
        $code = $LASTEXITCODE
        $text = ($lines | ForEach-Object { "$_" }) -join "`n"
    } finally { $ErrorActionPreference = $previousPreference }
    if ($code -ne 0 -and !$AllowFailure) { throw "Command failed (exit $code): $Executable $($Arguments[0])`n$text" }
    return [pscustomobject]@{ Code = $code; Text = $text.Trim() }
}
function Git([string[]]$Arguments, [switch]$AllowFailure) {
    $prefix = @('-C', $root)
    if ($Proxy) { $prefix += @('-c', "http.proxy=$Proxy") }
    return Invoke-Tool $GitPath ($prefix + $Arguments) -AllowFailure:$AllowFailure
}
function Gh([string[]]$Arguments) {
    return (Invoke-Tool $GhPath $Arguments).Text
}
function Require([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Stream-Hash($Stream) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($Stream)).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
}
function Repository-Name([string]$Url) {
    if ($Url -match '^(?:https://github\.com/|git@github\.com:|ssh://git@github\.com/)([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+?)(?:\.git)?/?$') {
        return $Matches[1]
    }
    throw 'Remote must be a GitHub HTTPS or SSH repository URL without credentials.'
}
function Check-Snapshot {
    Require ((Git @('rev-parse', 'HEAD')).Text -eq $commit) 'HEAD changed during publishing; stopped.'
    Require (!(Git @('status', '--porcelain', '--untracked-files=normal')).Text) 'Workspace is dirty; commit changes first.'
}
function Runs {
    $response = Gh @('api', "repos/$repository/actions/runs?head_sha=$commit") | ConvertFrom-Json
    return @($response.workflow_runs | Where-Object {
        $_.head_sha -eq $commit -and $_.head_branch -ceq $refName -and $_.path -eq $workflow
    })
}
function Verify-Release {
    $release = Gh @('api', "repos/$repository/releases/tags/$tag") | ConvertFrom-Json
    Require ($release.tag_name -ceq $tag -and !$release.draft -and !$release.prerelease) 'Release is not a matching public stable release.'
    $expected = @('MusicPlayer2.zip', 'MusicPlayer2.zip.sha256', 'client-release-manifest.json')
    $actual = @($release.assets | ForEach-Object { $_.name })
    Require ($actual.Count -eq 3 -and !(Compare-Object ($expected | Sort-Object) ($actual | Sort-Object))) 'Release assets are incomplete or unexpected.'
    foreach ($asset in $release.assets) { Require ($asset.size -gt 0) 'A release asset is empty.' }
    $directory = Join-Path $root ('test-results/publish/' + $tag + '-' + [guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $directory -Force
    $null = Gh @('release', 'download', $tag, '--repo', $repository, '--dir', $directory,
        '--pattern', 'MusicPlayer2.zip', '--pattern', 'MusicPlayer2.zip.sha256', '--pattern', 'client-release-manifest.json')
    $manifest = Get-Content -LiteralPath (Join-Path $directory 'client-release-manifest.json') -Raw | ConvertFrom-Json
    Require ($manifest.version -ceq $config.version -and $manifest.commit -ceq $commit -and $manifest.platform -ceq 'windows-x64') 'Release manifest identity mismatch.'
    foreach ($field in @('repository', 'commit', 'version', 'schema')) {
        Require ($manifest.server_baseline.$field -ceq $config.('server_' + $field)) 'Release server baseline mismatch.'
    }
    Require (@($manifest.assets).Count -eq 1 -and $manifest.assets[0].name -ceq 'MusicPlayer2.zip') 'Manifest asset mismatch.'
    $archivePath = Join-Path $directory 'MusicPlayer2.zip'
    $inputStream = [IO.File]::OpenRead($archivePath)
    try { $hash = Stream-Hash $inputStream } finally { $inputStream.Dispose() }
    Require ($hash -ceq $manifest.assets[0].sha256 -and (Get-Item -LiteralPath $archivePath).Length -eq $manifest.assets[0].size) 'Downloaded ZIP hash or size mismatch.'
    $checksum = (Get-Content -LiteralPath (Join-Path $directory 'MusicPlayer2.zip.sha256') -Raw).Trim()
    Require ($checksum -ceq "$hash  MusicPlayer2.zip") 'Release checksum file mismatch.'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        $entry = $archive.GetEntry('MusicPlayer2/BUILD-INFO.txt')
        Require ($null -ne $entry) 'ZIP build information is missing.'
        $reader = [IO.StreamReader]::new($entry.Open())
        try { $information = $reader.ReadToEnd() } finally { $reader.Dispose() }
        $fields = @{}
        foreach ($line in ($information -split '\r?\n')) {
            if ($line -match '^([^:]+): (.+)$') { $fields[$Matches[1]] = $Matches[2] }
        }
        Require ($fields['Git'] -ceq $commit -and $fields['Release'] -ceq $config.version -and $fields['Platform'] -ceq 'windows-x64') 'ZIP build identity mismatch.'
        $executable = $archive.GetEntry('MusicPlayer2/MusicPlayer2.exe')
        Require ($null -ne $executable) 'ZIP executable is missing.'
        $stream = $executable.Open()
        try { $exeHash = Stream-Hash $stream } finally { $stream.Dispose() }
        Require ($exeHash -ceq $manifest.executable_sha256 -and $exeHash -ceq $fields['MusicPlayer2.exe SHA-256']) 'ZIP executable hash mismatch.'
    } finally { $archive.Dispose() }
    Write-Host "RELEASE VERIFIED: $($release.html_url)"
    Write-Host "Verified downloads: $directory"
}

try {
    $GitPath = (Get-Command $GitPath -ErrorAction Stop).Source
    Require (([IO.Path]::GetFullPath((Git @('rev-parse', '--show-toplevel')).Text)).TrimEnd('\', '/') -eq $root.TrimEnd('\', '/')) 'Script must be in the client repository root.'
    Require ((Git @('rev-parse', '--is-shallow-repository')).Text -eq 'false') 'Shallow clone: complete origin history before publishing.'
    $branch = (Git @('symbolic-ref', '--quiet', '--short', 'HEAD')).Text
    Require ($branch -cin @('master', 'main')) 'Current branch must be master or main; detached HEAD is unsupported.'
    $commit = (Git @('rev-parse', 'HEAD')).Text
    Check-Snapshot
    $fetchUrls = @((Git @('remote', 'get-url', '--all', $Remote)).Text -split '\r?\n')
    $pushUrls = @((Git @('remote', 'get-url', '--push', '--all', $Remote)).Text -split '\r?\n')
    Require ($fetchUrls.Count -eq 1 -and $pushUrls.Count -eq 1) 'Remote must have exactly one fetch URL and one push URL.'
    $repository = Repository-Name $fetchUrls[0]
    Require ($repository -ceq (Repository-Name $pushUrls[0])) 'Fetch and push remotes differ.'
    Require (Test-Path -LiteralPath (Join-Path $root $workflow)) 'Client Actions workflow is missing.'
    $config = Get-Content -LiteralPath (Join-Path $root 'client-release.json') -Raw | ConvertFrom-Json
    Require ($config.version -cmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') 'Invalid client release version.'
    if ($Mode -eq 'Build') { Require (!$Version) '-Version applies only to Release mode.' }
    else {
        if ($Version) { Require ($Version -ceq $config.version) 'Requested version differs from client-release.json.' }
        $Version = $config.version
        $tag = 'v' + $Version
        $notes = Get-Content -LiteralPath (Join-Path $root 'Documents/client-release-notes.md') -Encoding UTF8
        Require ($notes[0] -match ('(?<![0-9.])' + [regex]::Escape($Version) + '(?![0-9.])')) 'Release notes version mismatch.'
        $localTag = Git @('show-ref', '--verify', '--quiet', "refs/tags/$tag") -AllowFailure
        Require ($localTag.Code -eq 1) 'Local release tag already exists; never overwrite a version tag.'
    }
    $branchRef = "refs/heads/$branch"
    $refName = if ($Mode -eq 'Release') { $tag } else { $branch }
    Write-Host "Target: $repository | mode: $Mode | branch: $branch | commit: $commit"
    if ($DryRun) {
        Write-Host "DRY RUN: validate remote permissions and fast-forward; push $branchRef"
        if ($tag) { Write-Host "DRY RUN: create $tag at $commit; atomically push branch and tag; await Actions and verify release assets" }
        else { Write-Host 'DRY RUN: await Actions build (no formal release)' }
        exit 0
    }
    $GhPath = (Get-Command $GhPath -ErrorAction Stop).Source
    $env:GH_HOST = 'github.com'
    if ($Proxy) {
        Require ($Proxy -match '^https?://[^\s]+$') 'Proxy must be an HTTP/HTTPS URL.'
        $env:HTTPS_PROXY = $Proxy
    }
    if (!$env:GH_TOKEN -and !$env:GITHUB_TOKEN) {
        $credential = Invoke-Tool $GitPath @('-C', $root, '-c', 'credential.interactive=false', 'credential', 'fill') -AllowFailure -InputText "protocol=https`nhost=github.com`npath=$repository.git`n`n"
        $password = @($credential.Text -split '\r?\n' | Where-Object { $_.StartsWith('password=') })
        if ($credential.Code -eq 0 -and $password.Count -gt 0) { $env:GH_TOKEN = $password[0].Substring(9) }
        else {
            $authentication = Invoke-Tool $GhPath @('auth', 'token') -AllowFailure
            Require ($authentication.Code -eq 0 -and !!$authentication.Text) 'GitHub login required: gh auth login, then gh auth setup-git.'
            $env:GH_TOKEN = $authentication.Text
            $authentication = $null
        }
        $credential = $null; $password = $null
    }
    if (!$env:GH_CONFIG_DIR) { $env:GH_CONFIG_DIR = Join-Path $root 'test-results/publish/github-cli' }
    $permission = Gh @('repo', 'view', $repository, '--json', 'viewerPermission') | ConvertFrom-Json
    Require ($permission.viewerPermission -in @('ADMIN', 'MAINTAIN', 'WRITE')) 'No write permission to target repository.'
    $remoteHead = (Git @('ls-remote', '--heads', $Remote, $branchRef)).Text
    if ($remoteHead) {
        $remoteCommit = ($remoteHead -split '\s+')[0]
        $null = Git @('fetch', '--no-tags', $Remote, $branchRef)
        Require ((Git @('merge-base', '--is-ancestor', $remoteCommit, $commit) -AllowFailure).Code -eq 0) 'Remote branch is ahead or diverged; reconcile it before pushing.'
    } else { $remoteCommit = '' }
    if ($tag) {
        Require (!(Git @('ls-remote', '--tags', $Remote, "refs/tags/$tag", "refs/tags/$tag^{}")).Text) 'Remote release tag already exists.'
        $parsedReleases = Gh @('release', 'list', '--repo', $repository, '--limit', '100', '--json', 'tagName,isDraft,isPrerelease') | ConvertFrom-Json
        $releases = @($parsedReleases)
        foreach ($release in $releases) {
            Require ($release.tagName -cne $tag) 'Release with this version already exists.'
            if (!$release.isDraft -and !$release.isPrerelease -and $release.tagName -cmatch '^v([0-9]+\.[0-9]+\.[0-9]+)$') {
                Require ([version]$Version -gt [version]$Matches[1]) 'Release version must exceed published stable versions.'
            }
        }
    }
    $previousRuns = @(Runs | ForEach-Object { $_.id })
    Check-Snapshot
    if ($Mode -eq 'Build' -and $remoteCommit -ceq $commit) {
        $null = Gh @('workflow', 'run', 'windows-release.yml', '--repo', $repository, '--ref', $branch)
        $expectedEvent = 'workflow_dispatch'
        Write-Host 'Remote already matches HEAD; requested a fresh Actions build.'
    } else {
        $pushArguments = @('push', '--atomic', '--no-follow-tags', '--set-upstream', $Remote, "${branchRef}:$branchRef")
        if ($tag) {
            $null = Git @('tag', '-a', $tag, '-m', "MusicPlayer2 $tag", $commit)
            $localTagCreated = $true
            $pushArguments += "refs/tags/${tag}:refs/tags/$tag"
        }
        $null = Git $pushArguments
        $pushed = $true
        $expectedEvent = 'push'
        Write-Host 'PUSH SUCCEEDED.'
    }
    if ($NoWait) {
        Write-Host "TRIGGERED ONLY (not verified): https://github.com/$repository/actions"
        exit 0
    }
    Write-Host 'Waiting for the matching Actions run.'
    $deadline = [DateTime]::UtcNow.AddMinutes($TimeoutMinutes)
    $lastStatus = ''
    $run = $null
    do {
        $candidates = @(Runs | Where-Object { $_.id -notin $previousRuns -and $_.event -eq $expectedEvent } | Sort-Object id -Descending)
        if ($candidates.Count -gt 0) {
            $run = $candidates[0]
            $status = "$($run.id): $($run.status) $($run.conclusion)"
            if ($status -cne $lastStatus) { Write-Host "$status | $($run.html_url)"; $lastStatus = $status }
            if ($run.status -eq 'completed') { break }
        }
        Require ([DateTime]::UtcNow -lt $deadline) "Timed out awaiting Actions. Check https://github.com/$repository/actions"
        Start-Sleep -Seconds $PollSeconds
    } while ($true)
    Require ($run.conclusion -eq 'success') "Actions failed: $($run.html_url). Remote refs are retained; no rollback was attempted."
    $details = Gh @('run', 'view', [string]$run.id, '--repo', $repository, '--json', 'headSha,jobs') | ConvertFrom-Json
    Require ($details.headSha -ceq $commit) 'Actions commit mismatch.'
    $requiredJobs = @('test', 'build-x86', 'verify-artifact')
    if ($tag) { $requiredJobs += 'release' }
    foreach ($jobName in $requiredJobs) {
        $jobs = @($details.jobs | Where-Object { $_.name -ceq $jobName })
        Require ($jobs.Count -eq 1 -and $jobs[0].conclusion -eq 'success') "Required Actions job did not pass: $jobName"
    }
    Write-Host "ACTIONS VERIFIED: $($run.html_url)"
    if ($tag) { Verify-Release }
    else { Write-Host 'BUILD SUCCEEDED: download MusicPlayer2-windows-x64 from Actions artifacts. No formal Release was published.' }
    exit 0
} catch {
    Write-Host "PUBLISH FAILED: $($_.Exception.Message)" -ForegroundColor Red
    if ($pushed) { Write-Host 'Push completed earlier; inspect Actions before retrying. Remote refs were not rolled back.' }
    if ($localTagCreated) { Write-Host "Local tag retained: $tag. Never overwrite an existing remote release tag." }
    exit 1
} finally {
    foreach ($name in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process') }
}
