param([ValidateSet('Debug', 'Release')][string]$Config = 'Release')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build-parity-msvc/src/parity/$Config"
$desktop = Join-Path $build 'lexica_desktop.exe'
$server = Join-Path $build 'lexica_parity_server.exe'
if (-not (Test-Path -LiteralPath $desktop)) {
  cmake --preset parity-msvc
  if ($LASTEXITCODE) { throw 'CMake configure failed' }
  cmake --build (Join-Path $root 'build-parity-msvc') --config $Config --target lexica_desktop lexica_parity_server
  if ($LASTEXITCODE) { throw 'CMake build failed' }
}
$ui = Join-Path $root 'ui/build/index.html'
if (-not (Test-Path -LiteralPath $ui)) {
  Push-Location (Join-Path $root 'ui')
  try { npm run build; if ($LASTEXITCODE) { throw 'UI build failed' } }
  finally { Pop-Location }
}
$stage = Join-Path $root 'dist/LexicaReproduce'
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -LiteralPath $desktop, $server -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'ui/build') -Destination (Join-Path $stage 'ui') -Recurse
$licenses = Join-Path $root 'third_party/licenses'
if (Test-Path -LiteralPath $licenses) {
  Copy-Item -LiteralPath $licenses -Destination (Join-Path $stage 'licenses') -Recurse
}
$version = (Get-Content -Raw -LiteralPath (Join-Path $root 'contracts/commands.json') | ConvertFrom-Json).contract_version
Set-Content -LiteralPath (Join-Path $stage 'VERSION.txt') -Value "LexicaReproduce $version`r`nWebView2 Runtime required`r`n" -Encoding ascii
Write-Output $stage
