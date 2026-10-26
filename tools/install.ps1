param(
  [Parameter(Mandatory = $true)][string]$Source,
  [string]$Destination = $(Join-Path $env:LOCALAPPDATA 'LexicaReproduce\app'),
  [switch]$NoShortcut
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installation.ps1')
$Source = [IO.Path]::GetFullPath($Source).TrimEnd('\', '/')
$Destination = Get-LexicaInstallPath $Destination
if ($Source -eq $Destination -or $Source.StartsWith($Destination + '\', [StringComparison]::OrdinalIgnoreCase) -or $Destination.StartsWith($Source + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Package and installation directories must be separate' }
if (-not (Test-Path -LiteralPath (Join-Path $Source 'lexica_desktop.exe'))) { throw 'Package source is missing lexica_desktop.exe' }
if (Get-ChildItem -LiteralPath $Source -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint } | Select-Object -First 1) { throw 'Package cannot contain links or junctions' }
& (Join-Path $PSScriptRoot 'check-runtime.ps1')
if (-not $?) { throw 'WebView2 runtime is required' }
if (Test-Path -LiteralPath $Destination) {
  if (Test-Path -LiteralPath (Join-Path $Destination '.lexica-install.json')) { Remove-LexicaOwnedFiles $Destination }
  elseif (Get-ChildItem -LiteralPath $Destination -Force | Select-Object -First 1) { throw 'Refusing to overwrite a nonempty directory without an installation manifest' }
}
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$files = @(Get-ChildItem -LiteralPath $Source -File -Recurse -Force | Where-Object Name -ne '.lexica-install.json')
# Record ownership before copying, so an interrupted install can be removed safely.
$relative = @($files | ForEach-Object { $_.FullName.Substring($Source.Length + 1) })
foreach ($name in $relative) {
  if (Test-Path -LiteralPath (Join-Path $Destination $name)) { throw "Refusing to overwrite an unowned file: $name" }
}
@{product = 'LexicaReproduce'; files = $relative} | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $Destination '.lexica-install.json') -Encoding utf8
foreach ($file in $files) {
  $target = Join-Path $Destination $file.FullName.Substring($Source.Length + 1)
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
  Copy-Item -LiteralPath $file.FullName -Destination $target -Force
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
