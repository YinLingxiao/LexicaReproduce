param([ValidateSet('Debug', 'Release')][string]$Config = 'Release')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not (Get-Command node -ErrorAction SilentlyContinue)) { $env:PATH = 'C:\Program Files\nodejs;' + $env:PATH }
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { $env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH }
$cmake = (Get-Command cmake).Source
Push-Location $root
try {
$build = Join-Path $root "build-parity-msvc/src/parity/$Config"
$desktop = Join-Path $build 'lexica_desktop.exe'
$server = Join-Path $build 'lexica_parity_server.exe'
  & $cmake --preset parity-msvc
  if ($LASTEXITCODE -lt 0 -and (Test-Path -LiteralPath 'C:\msys64\ucrt64\bin\cmake.exe')) {
    $cmake = 'C:\msys64\ucrt64\bin\cmake.exe'
    & $cmake --preset parity-msvc
  }
  if ($LASTEXITCODE) { throw 'CMake configure failed' }
  & $cmake --build (Join-Path $root 'build-parity-msvc') --config $Config --target lexica_desktop lexica_parity_server --parallel 3
  if ($LASTEXITCODE) { throw 'CMake build failed' }
$ui = Join-Path $root 'ui/build/index.html'
  Push-Location (Join-Path $root 'ui')
  try { npm run build; if ($LASTEXITCODE) { throw 'UI build failed' } }
  finally { Pop-Location }
$stage = Join-Path $root 'dist/LexicaReproduce'
$stage = [IO.Path]::GetFullPath($stage)
$expectedParent = [IO.Path]::GetFullPath((Join-Path $root 'dist'))
if ((Split-Path -Parent $stage) -ne $expectedParent) { throw 'Package directory escapes dist' }
if ((Test-Path -LiteralPath $expectedParent) -and ((Get-Item -LiteralPath $expectedParent -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'dist cannot be a link' }
if ((Test-Path -LiteralPath $stage) -and ((Get-Item -LiteralPath $stage -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Package directory cannot be a link' }
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -LiteralPath $desktop, $server -Destination $stage
foreach ($script in @('install.ps1', 'uninstall.ps1', 'installation.ps1', 'check-runtime.ps1', 'dependencies.json')) {
  Copy-Item -LiteralPath (Join-Path $PSScriptRoot $script) -Destination $stage
}
Copy-Item -LiteralPath (Join-Path $root 'ui/build') -Destination (Join-Path $stage 'ui') -Recurse
$licenses = Join-Path $root 'third_party/licenses'
if (Test-Path -LiteralPath $licenses) {
  Copy-Item -LiteralPath $licenses -Destination (Join-Path $stage 'licenses') -Recurse
}
$version = (Get-Content -Raw -LiteralPath (Join-Path $root 'contracts/commands.json') | ConvertFrom-Json).contract_version
Set-Content -LiteralPath (Join-Path $stage 'VERSION.txt') -Value "LexicaReproduce $version`r`nWebView2 Runtime required`r`n" -Encoding ascii
Write-Output $stage
} finally { Pop-Location }
