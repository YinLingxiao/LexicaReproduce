param(
  [string]$Destination = $(Join-Path $env:LOCALAPPDATA 'LexicaReproduce\app'),
  [switch]$DeleteData
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installation.ps1')
$Destination = Get-LexicaInstallPath $Destination
if (Test-Path -LiteralPath $Destination) { Remove-LexicaOwnedFiles $Destination }
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'Lexica Reproduce.lnk'
if (Test-Path -LiteralPath $shortcut) {
  $shell = New-Object -ComObject WScript.Shell
  if ($shell.CreateShortcut($shortcut).TargetPath -eq (Join-Path $Destination 'lexica_desktop.exe')) { Remove-Item -LiteralPath $shortcut -Force }
}
$data = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'LexicaReproduce\parity-v1'))
if ($DeleteData) {
  # Fixed product data directory, never a user-supplied or manifest-derived path.
  $parent = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'LexicaReproduce'))
  if ((Split-Path -Parent $data) -ne $parent) { throw 'Invalid personal data directory' }
  if (Test-Path -LiteralPath $data) {
    if ((Get-Item -LiteralPath $data -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Personal data directory cannot be a link' }
    Remove-Item -LiteralPath $data -Recurse -Force
  }
  Write-Output 'Application and personal data removed'
} else { Write-Output "Application removed. Personal data kept at $data" }
