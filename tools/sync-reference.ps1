$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskManifest = Get-Content -Raw -LiteralPath (Join-Path $taskRoot 'contracts/commands.json') | ConvertFrom-Json
$taskReference = Join-Path $taskRoot 'reference/lexica'
if (-not (Test-Path -LiteralPath (Join-Path $taskReference '.git'))) {
    git clone --no-checkout 'https://github.com/YinLingxiao/Lexica..git' $taskReference
    if ($LASTEXITCODE) { throw 'Reference clone failed' }
    git -C $taskReference checkout --detach $taskManifest.upstream_sha
    if ($LASTEXITCODE) { throw 'Cannot check out the pinned reference' }
}
$taskHead = git -C $taskReference rev-parse HEAD 2>$null
if ($taskHead -ne $taskManifest.upstream_sha) {
    $taskDirty = git -C $taskReference status --porcelain
    if ($taskDirty) { throw 'Reference checkout contains local changes' }
    git -C $taskReference checkout --detach $taskManifest.upstream_sha
    if ($LASTEXITCODE) { throw 'Cannot check out the pinned reference' }
}
Write-Output ('Reference SHA: ' + (git -C $taskReference rev-parse HEAD))
