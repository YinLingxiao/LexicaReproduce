$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskLock = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'dependencies.json') | ConvertFrom-Json
$taskSdk = Join-Path $taskRoot ('.deps/webview2/' + $taskLock.webview2.version)
if (-not (Test-Path -LiteralPath (Join-Path $taskSdk 'build/native/include/WebView2.h'))) {
    $taskZip = Join-Path $env:TEMP ('lexica-webview2-' + [guid]::NewGuid().ToString() + '.zip')
    Invoke-WebRequest -Uri $taskLock.webview2.url -OutFile $taskZip
    $taskHash = (Get-FileHash -LiteralPath $taskZip -Algorithm SHA256).Hash
    if ($taskHash -ne $taskLock.webview2.sha256) { throw 'WebView2 SDK hash mismatch' }
    New-Item -ItemType Directory -Force -Path $taskSdk | Out-Null
    Expand-Archive -LiteralPath $taskZip -DestinationPath $taskSdk -Force
    Remove-Item -LiteralPath $taskZip
}
Push-Location (Join-Path $taskRoot 'ui')
try { npm ci; if ($LASTEXITCODE) { throw 'npm ci failed' } }
finally { Pop-Location }
node (Join-Path $PSScriptRoot 'generate-contracts.mjs') --check
if ($LASTEXITCODE) { throw 'Generated contract files are stale' }
Write-Output 'Dependencies ready. Run: cmake --preset parity-msvc'
