param([string]$Desktop, [string]$Ui)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Desktop) { $Desktop = Join-Path $root 'build-parity-msvc/src/parity/Debug/lexica_desktop.exe' }
if (-not $Ui) { $Ui = Join-Path $root 'ui/build' }
$run = Join-Path $root ('build-parity-msvc/test-output/startup-responsive/' + [guid]::NewGuid().ToString())
New-Item -ItemType Directory -Force -Path $run | Out-Null
$lockScript = Join-Path $run 'hold-writer.py'
@'
import sqlite3,sys,time
from pathlib import Path
data,root=map(Path,sys.argv[1:])
c=sqlite3.connect(data/'lexica.db')
for path in sorted((root/'src/parity/storage/migrations').glob('*.sql')):
 c.executescript(path.read_text(encoding='utf-8'))
c.execute('PRAGMA user_version=6');c.commit()
c.execute('PRAGMA journal_mode=WAL')
c.execute('BEGIN IMMEDIATE')
(data/'writer-ready').touch()
deadline=time.monotonic()+15
while not (data/'release-writer').exists() and time.monotonic()<deadline: time.sleep(.02)
c.rollback();c.close()
'@ | Set-Content -LiteralPath $lockScript -Encoding utf8
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class StartupWindow {
 public delegate bool Visitor(IntPtr w,IntPtr p);
 [DllImport("user32.dll")] static extern bool EnumWindows(Visitor v,IntPtr p);
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr w,out uint p);
 [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr w,uint m,IntPtr a,IntPtr b,uint f,uint t,out IntPtr r);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w,uint m,IntPtr a,IntPtr b);
 public static IntPtr Find(uint process) {
  IntPtr found=IntPtr.Zero;
  EnumWindows((w,p)=>{uint id;GetWindowThreadProcessId(w,out id);if(id==process){found=w;return false;}return true;},IntPtr.Zero);
  return found;
 }
}
'@
$python = if ($env:LEXICA_TEST_PYTHON) { $env:LEXICA_TEST_PYTHON } else { (Get-Command python).Source }
$pythonArgs = @()
$writer = $null
$desktopProcess = $null
function Join-Arguments([string[]]$Items) { ($Items | ForEach-Object { '"' + $_ + '"' }) -join ' ' }
try {
 $writerArguments = Join-Arguments ($pythonArgs + @($lockScript,$run,$root))
 $writer = Start-Process -FilePath $python -ArgumentList $writerArguments -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $run 'writer-error.log')
 $deadline = [DateTime]::UtcNow.AddSeconds(10)
 while (-not (Test-Path -LiteralPath (Join-Path $run 'writer-ready'))) {
  if ($writer.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw 'Writer fixture did not start' }
  Start-Sleep -Milliseconds 50
 }
 $desktopArguments = Join-Arguments @('--data-dir',$run,'--ui-dir',$Ui,'--no-auto-setup')
 $desktopProcess = Start-Process -FilePath $Desktop -ArgumentList $desktopArguments -PassThru -WindowStyle Hidden
 $window = [IntPtr]::Zero
 $deadline = [DateTime]::UtcNow.AddSeconds(3)
 while ($window -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline) {
  $window = [StartupWindow]::Find($desktopProcess.Id)
  Start-Sleep -Milliseconds 30
 }
 if ($window -eq [IntPtr]::Zero) { throw 'Startup window missing' }
 $worstMs = 0.0
 for ($i = 0; $i -lt 5; ++$i) {
  $reply = [IntPtr]::Zero
  $watch = [Diagnostics.Stopwatch]::StartNew()
  $ok = [StartupWindow]::SendMessageTimeout($window,0,[IntPtr]::Zero,[IntPtr]::Zero,2,250,[ref]$reply)
  $watch.Stop()
  $worstMs = [Math]::Max($worstMs,$watch.Elapsed.TotalMilliseconds)
  if ($ok -eq [IntPtr]::Zero) { throw 'UI message loop blocked while database writer was held' }
  Start-Sleep -Milliseconds 100
 }
 [void][StartupWindow]::PostMessage($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
 New-Item -ItemType File -Path (Join-Path $run 'release-writer') | Out-Null
 if (-not $desktopProcess.WaitForExit(5000)) { throw 'Closing during startup did not finish' }
 if ($desktopProcess.ExitCode -ne 0) { throw 'Startup close returned an error' }
 $writer.WaitForExit()
 $report = Join-Path $run 'smoke.json'
 $normalArguments = Join-Arguments @('--data-dir',$run,'--ui-dir',$Ui,'--smoke-report',$report)
 $normal = Start-Process -FilePath $Desktop -ArgumentList $normalArguments -PassThru -WindowStyle Hidden
 if (-not $normal.WaitForExit(20000)) { $normal.Kill(); throw 'Normal startup smoke timed out' }
 $smoke = Get-Content -Raw -LiteralPath $report | ConvertFrom-Json
 if (-not $smoke.ok) { throw 'Normal startup smoke failed' }
 $result = @{ok=$true; writer_lock_ui_responsive=$true; close_during_startup=$true; max_window_reply_ms=[Math]::Round($worstMs,2); normal_backend_startup_ms=$smoke.backend_startup_ms}
 $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'report.json') -Encoding utf8
 $result | ConvertTo-Json -Compress
 Write-Output $run
} finally {
 if ($desktopProcess -and -not $desktopProcess.HasExited) { $desktopProcess.Kill() }
 if ($writer -and -not $writer.HasExited) { New-Item -ItemType File -Force -Path (Join-Path $run 'release-writer') | Out-Null; $writer.WaitForExit(3000) | Out-Null }
}
exit 0
