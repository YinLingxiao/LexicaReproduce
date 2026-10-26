param([switch]$SkipUi, [switch]$Smoke)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
if (-not (Get-Command node -ErrorAction SilentlyContinue)) { $env:PATH = 'C:\Program Files\nodejs;' + $env:PATH }
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    if (Test-Path -LiteralPath 'C:\msys64\ucrt64\bin\cmake.exe') { $env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH }
    else { throw 'Install CMake 3.21+ and put cmake on PATH' }
}
function Invoke-Checked([scriptblock]$Task) { & $Task; if ($LASTEXITCODE) { throw "Command failed with exit code $LASTEXITCODE" } }
Push-Location $taskRoot
try {
    Invoke-Checked { node tools/generate-contracts.mjs --check }
    Invoke-Checked { node tools/verify-baseline.mjs }
    if (-not $SkipUi) {
        Push-Location ui
        try { Invoke-Checked { npm run check }; Invoke-Checked { npm run build } } finally { Pop-Location }
    }
    Invoke-Checked { cmake --preset parity-msvc }
    Invoke-Checked { cmake --build --preset parity-msvc --parallel 3 }
    Invoke-Checked { ctest --preset parity-msvc }
    $taskOutput = Join-Path $taskRoot 'build-parity-msvc/src/parity/Debug'
    if (-not (Test-Path -LiteralPath 'ui/build/index.html')) { throw 'Build ui before packaging' }
    New-Item -ItemType Directory -Force -Path (Join-Path $taskOutput 'ui') | Out-Null
    Copy-Item -Path 'ui/build/*' -Destination (Join-Path $taskOutput 'ui') -Recurse -Force
    if ($Smoke) {
        $taskRun = Join-Path $taskRoot ('build-parity-msvc/test-output/desktop/' + [guid]::NewGuid().ToString())
        New-Item -ItemType Directory -Force -Path $taskRun | Out-Null
        $taskExe = Join-Path $taskOutput 'lexica_desktop.exe'
        $taskReport = Join-Path $taskRun 'report.json'
        $taskData = Join-Path $taskRun 'data'
        $taskProcess = Start-Process -FilePath $taskExe -ArgumentList @('--data-dir', ('"' + $taskData + '"'), '--smoke-report', ('"' + $taskReport + '"')) -WindowStyle Hidden -PassThru
        if (-not $taskProcess.WaitForExit(45000)) { Stop-Process -Id $taskProcess.Id; throw 'Desktop smoke timed out' }
        if ($taskProcess.ExitCode -ne 0) { if (Test-Path -LiteralPath $taskReport) { Get-Content -LiteralPath $taskReport }; throw 'Desktop smoke failed' }
        $taskResult = Get-Content -Raw -LiteralPath $taskReport | ConvertFrom-Json
        if (-not $taskResult.ok -or -not $taskResult.ui_rendered) { throw 'Desktop smoke result is incomplete' }
        Write-Output ('Desktop smoke: ' + $taskReport)
    }
    Write-Output ('Desktop executable: ' + (Join-Path $taskOutput 'lexica_desktop.exe'))
} finally { Pop-Location }
