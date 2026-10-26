param(
  [string]$Destination = $(Join-Path $env:LOCALAPPDATA 'LexicaReproduce\app'),
  [switch]$DeleteData
)
$ErrorActionPreference = 'Stop'
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'Lexica Reproduce.lnk'
if (Test-Path -LiteralPath $shortcut) { Remove-Item -LiteralPath $shortcut -Force }
if (Test-Path -LiteralPath $Destination) { Remove-Item -LiteralPath $Destination -Recurse -Force }
$data = Join-Path $env:LOCALAPPDATA 'LexicaReproduce\parity-v1'
if ($DeleteData) {
  if (Test-Path -LiteralPath $data) { Remove-Item -LiteralPath $data -Recurse -Force }
  Write-Output 'Application and personal data removed'
} else {
  Write-Output "Application removed. Personal data kept at $data"
}
