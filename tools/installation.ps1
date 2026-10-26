# Shared installation ownership checks. Never remove files outside this manifest.
function Get-LexicaInstallPath([string]$Path) {
  $full = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
  if ($full -eq [IO.Path]::GetPathRoot($full).TrimEnd('\', '/')) { throw 'A drive root cannot be an installation directory' }
  $dataRoot = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'LexicaReproduce\parity-v1'))
  if ($full -eq $dataRoot -or $dataRoot.StartsWith($full + '\', [StringComparison]::OrdinalIgnoreCase) -or $full.StartsWith($dataRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Installation directory overlaps personal data' }
  $cursor = $full
  while ($cursor) {
    if (Test-Path -LiteralPath $cursor) {
      if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Installation paths cannot contain links or junctions' }
    }
    $cursor = Split-Path -Parent $cursor
  }
  return $full
}
function Get-LexicaOwnedFiles([string]$Destination) {
  $marker = Join-Path $Destination '.lexica-install.json'
  if (-not (Test-Path -LiteralPath $marker)) { throw 'Directory is not owned by the Lexica installer' }
  $manifest = Get-Content -Raw -LiteralPath $marker | ConvertFrom-Json
  if ($manifest.product -ne 'LexicaReproduce') { throw 'Invalid installation manifest' }
  $paths = @()
  foreach ($file in $manifest.files) {
    if ([IO.Path]::IsPathRooted($file)) { throw 'Invalid installation manifest path' }
    $target = [IO.Path]::GetFullPath((Join-Path $Destination $file))
    if (-not $target.StartsWith($Destination + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Manifest path escapes installation directory' }
    if (Test-Path -LiteralPath $target) {
      $item = Get-Item -LiteralPath $target -Force
      if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Installed file was replaced by a directory or link' }
      $parent = Split-Path -Parent $target
      while ($parent -ne $Destination) {
        if ((Get-Item -LiteralPath $parent -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Installed directory was replaced by a link' }
        $parent = Split-Path -Parent $parent
      }
      $paths += $target
    }
  }
  return $paths
}
function Remove-LexicaOwnedFiles([string]$Destination) {
  $paths = @(Get-LexicaOwnedFiles $Destination)
  foreach ($file in $paths) { Remove-Item -LiteralPath $file -Force }
  Remove-Item -LiteralPath (Join-Path $Destination '.lexica-install.json') -Force
  # Preserve unrecognized files and remove only empty directories, without recursion.
  $directories = @($paths | ForEach-Object {
    $parent = Split-Path -Parent $_
    while ($parent -and $parent.StartsWith($Destination + '\', [StringComparison]::OrdinalIgnoreCase)) {
      $parent
      $parent = Split-Path -Parent $parent
    }
  } | Sort-Object -Unique | Sort-Object Length -Descending)
  foreach ($dir in $directories + @($Destination)) {
    if ((Test-Path -LiteralPath $dir) -and -not (Get-ChildItem -LiteralPath $dir -Force | Select-Object -First 1)) { Remove-Item -LiteralPath $dir -Force }
  }
}
