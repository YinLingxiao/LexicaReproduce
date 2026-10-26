$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'dependencies.json') | ConvertFrom-Json
$client = $lock.webview2Runtime.product
$keys = @(
  "HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\$client",
  "HKLM:\SOFTWARE\Microsoft\EdgeUpdate\Clients\$client",
  "HKCU:\SOFTWARE\Microsoft\EdgeUpdate\Clients\$client"
)
foreach ($key in $keys) {
  if (-not (Test-Path -LiteralPath $key)) { continue }
  $version = (Get-ItemProperty -LiteralPath $key -Name pv -ErrorAction SilentlyContinue).pv
  if ($version -and $version -ne '0.0.0.0') {
    Write-Output "WebView2 Runtime $version"
    exit 0
  }
}
Write-Error "WebView2 Runtime is not installed. $($lock.webview2Runtime.installer)"
exit 1
