param(
  [Parameter(Mandatory = $true)][string]$Source,
  [string]$Destination = $(Join-Path $env:LOCALAPPDATA 'LexicaReproduce\app'),
  [switch]$NoShortcut
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath (Join-Path $Source 'lexica_desktop.exe'))) {
  throw 'Package source is missing lexica_desktop.exe'
}
& (Join-Path $PSScriptRoot 'check-runtime.ps1')
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
Copy-Item -Path (Join-Path $Source '*') -Destination $Destination -Recurse -Force
$versionFile = Join-Path $Source 'VERSION.txt'
if (Test-Path -LiteralPath $versionFile) {
  Copy-Item -LiteralPath $versionFile -Destination (Join-Path $Destination 'installed-version.txt') -Force
}
if (-not $NoShortcut) {
  $shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'Lexica Reproduce.lnk'
  $shell = New-Object -ComObject WScript.Shell
  $link = $shell.CreateShortcut($shortcut)
  $link.TargetPath = Join-Path $Destination 'lexica_desktop.exe'
  $link.WorkingDirectory = $Destination
  $link.Description = 'Lexica Reproduce'
  $link.Save()
}
Write-Output $Destination
