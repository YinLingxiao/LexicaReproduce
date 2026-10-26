param([Parameter(Mandatory = $true)][string]$Package)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$run = Join-Path $repo ('build-parity-msvc/test-output/install/' + [guid]::NewGuid().ToString())
$destination = Join-Path $run '中文安装目录'
$keep = Join-Path $destination 'personal-sentinel.txt'
New-Item -ItemType Directory -Force -Path $run | Out-Null
& (Join-Path $PSScriptRoot 'install.ps1') -Source $Package -Destination $destination -NoShortcut
if (-not (Test-Path -LiteralPath (Join-Path $destination '.lexica-install.json'))) { throw 'Install manifest missing' }
Set-Content -LiteralPath $keep -Value 'preserve me'
& (Join-Path $PSScriptRoot 'install.ps1') -Source $Package -Destination $destination -NoShortcut
if ((Get-Content -Raw -LiteralPath $keep).Trim() -ne 'preserve me') { throw 'Upgrade lost an unowned file' }
& (Join-Path $PSScriptRoot 'uninstall.ps1') -Destination $destination
if (Test-Path -LiteralPath (Join-Path $destination 'lexica_desktop.exe')) { throw 'Uninstall kept the application' }
if (-not (Test-Path -LiteralPath $keep)) { throw 'Uninstall removed an unowned file' }
$refused = $false
try { & (Join-Path $PSScriptRoot 'uninstall.ps1') -Destination $destination } catch { $refused = $true }
if (-not $refused) { throw 'Uninstall accepted an unowned directory' }
$refused = $false
try { & (Join-Path $PSScriptRoot 'install.ps1') -Source $Package -Destination $destination -NoShortcut } catch { $refused = $true }
if (-not $refused) { throw 'Install accepted an unowned nonempty directory' }
$refused = $false
try { & (Join-Path $PSScriptRoot 'uninstall.ps1') -Destination ([IO.Path]::GetPathRoot($repo)) } catch { $refused = $true }
if (-not $refused) { throw 'Uninstall accepted a drive root' }
@{ok = $true; install = $true; upgrade = $true; uninstall = $true; unowned_files_preserved = $true; invalid_destinations_rejected = $true} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'report.json') -Encoding utf8
Write-Output ('Installer regression passed: ' + $run)
